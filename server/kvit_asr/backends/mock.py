"""The backend that makes the whole product testable with nothing installed.

This is infrastructure, not a toy. Every other backend in bench/CANDIDATES.md needs an
environment that does not exist yet (`envs/onnx`, `envs/ct2`, `envs/qwen-asr`) or a GPU that a CI
box will not have. The mock is what lets the named-pipe transport, the framing codec, the session
state machine, the stability computation, the commit engine, the anchor estimator and the injector
all be exercised end to end on a bare stdlib CPython — which is the only way plan.md §7's exit
gates ("no wrong-window injection under automated focus-race tests", "clean recovery from sidecar
crash") can be tested at all.

**Zero third-party imports.** Same rule as base.py, for the same reason, and here it is absolute:
if the mock ever needs numpy, the "runs on a bare interpreter" property that justifies its
existence is gone.

─── Why it must revise, and revise badly ───────────────────────────────────────────────────────

A mock that only ever appends is worse than no mock, because it makes a broken commit engine pass
every end-to-end test. plan.md §1.4 exists entirely because streaming ASR revises what it already
said; the invariant, the stability algorithm, the ICER metric and the violation-handling UX are
all responses to that fact. So this backend revises its tail on every emission, occasionally
reaches back further than usual, and — when explicitly asked — produces revisions that are
outright inadmissible.

─── Determinism ────────────────────────────────────────────────────────────────────────────────

Everything random here comes from ``random.Random`` seeded from ``MockConfig.seed`` and the
utterance index, so a failing end-to-end test reproduces from its seed alone. Per-utterance
reseeding means utterance *n* produces the same text whether or not utterance *n-1* ran, which
makes a test that exercises one utterance stable when a test that exercises three changes.

The determinism guarantee is "same seed, same CPython build, same sequence". CPython's Mersenne
Twister and the `random.random`/`randrange`/`choice` algorithms have been stable for many
releases, but this is a property of the interpreter, not a promise this file can enforce — a
future CPython that changed them would change the scripts. Tests here assert *self-consistency*
(two instances with the same seed agree) rather than golden strings, so they cannot rot that way.
"""

from __future__ import annotations

import enum
import os
import random
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Sequence, Union

from .base import (
    Backend,
    BackendCapabilities,
    BackendDecodeError,
    BackendLoadError,
    Hypothesis,
    ProgressFn,
    ResetReason,
    StreamingMode,
    bytes_to_ms,
    pcm_peak,
    utf8_len,
)

__all__ = [
    "MockBackend",
    "MockConfig",
    "SilenceBehaviour",
    "HostileMode",
    "DEFAULT_SCRIPT",
]


# ─── The default script ─────────────────────────────────────────────────────────────────────
#
# Chosen to hit the failure modes this project exists to fix and the ones its own code is most
# likely to have:
#
#   * technical identifiers with internal punctuation ("llama.cpp", "src/protocol.h",
#     "stable_bytes") — plan.md §4: *"raw is what matters for `llama.cpp` vs `llama dot cpp`"*,
#     and the perturbation table below implements exactly that mis-transcription;
#   * multi-byte text (café, €, naïve, résumé) so any offset bug shows up as a wrong byte count
#     rather than passing on ASCII and failing on a user's first accented word;
#   * a ZWJ emoji sequence and a skin-tone modifier, so downstream grapheme-cluster boundary
#     checks (docs/protocol.md §8.4, fixture 013) have something real to reject.

DEFAULT_SCRIPT: tuple[str, ...] = (
    "can you change the llama.cpp config to use Qwen3 instead of the default model",
    "open src/protocol.h and check whether stable_bytes is a byte offset or a code point index",
    "the café bill came to twelve € and the naïve résumé parser choked on it",
    "commit with the message fix UTF-8 offsets must land on grapheme cluster boundaries",
    "ship it 👍🏽 and tell the family 👨‍👩‍👧‍👦 we are done for the day",
)


class SilenceBehaviour(str, enum.Enum):
    """What the mock does when handed digital silence.

    plan.md §1.6 requires both halves of this to be testable: the server must VAD-gate the decoder
    during silence *because* several ASR models hallucinate on it, and the bake-off (§4) scores
    "hallucinated output on silence-only and noise-only input". A test for that needs a backend
    that can be made to hallucinate on demand, and a control that does not.
    """

    QUIET = "quiet"
    """Emit nothing. What a well-behaved gated pipeline should look like."""

    HALLUCINATE = "hallucinate"
    """Emit plausible filler over silence, the way Whisper does. ``wan-dancer``'s own
    ``notes/transcribe.py`` carries a comment about Whisper emitting "Thank you." / "Bye" over
    trailing silence with timestamps past the end of the audio (plan.md §1.6); the default
    ``hallucinations`` tuple below is that observation, reused as a test fixture."""


class HostileMode(str, enum.Enum):
    """**Deliberate protocol violations. Off by default. Never enable in a shipping build.**

    docs/protocol.md §8.3 specifies exactly what a receiver must do when the committed-prefix
    invariant is violated: preserve the document, abort the session (not the connection), send
    ``stability_violation``, log both texts and the diverging offset, and count an
    irrevocable-error event against that backend's ICER. That is a substantial amount of client
    behaviour whose input **no correct pipeline can produce** — the server's stability computation
    exists precisely to make it impossible. Without a backend that can be told to break the rule,
    that code path ships untested and is first exercised by a real model corrupting a real
    document.

    These modes drive ``Hypothesis.stable_bytes_override``, which the server must be explicitly
    told to honour. See that field's docstring.
    """

    OFF = "off"

    DECREASE = "decrease"
    """``stable_bytes`` goes backwards within an utterance — docs/protocol.md §8.2 requirement 3,
    first half; fixture 017. The obvious violation."""

    MUTATE = "mutate"
    """``stable_bytes`` never decreases, but the bytes *below* it change — §8.2 requirement 3,
    second half; fixture 018. **The one with teeth**, and the one an implementation is most likely
    to miss, because a receiver that only compares the numbers sees nothing wrong while text it
    may already have typed into the user's document has been retracted.

    The mutation is a case flip on the first ASCII letter, chosen because it changes bytes without
    changing byte *lengths* — so the offset stays exactly where it was and the failure is
    unambiguously "the prefix mutated", not "the offset moved"."""


# ─── Configuration ──────────────────────────────────────────────────────────────────────────

ScriptSource = Union[str, Sequence[str], None]


@dataclass
class MockConfig:
    """Everything about the mock's behaviour. All of it deterministic given ``seed``."""

    # ── What it says ────────────────────────────────────────────────────────────────────────
    script: ScriptSource = None
    """Inline script: one string (split on newlines) or a sequence of utterances. ``None`` uses
    ``DEFAULT_SCRIPT``. Ignored if ``script_path`` is set."""

    script_path: Union[str, os.PathLike, None] = None
    """A UTF-8 text file: one utterance per non-blank line, ``#`` comments ignored."""

    loop_script: bool = True
    """Wrap around at the end of the script. ``False`` makes later utterances empty, which is a
    useful way to test the server's ``audio_underrun`` path."""

    # ── What it declares ────────────────────────────────────────────────────────────────────
    model_id: str = "mock"
    mode: StreamingMode = StreamingMode.NATIVE_STREAMING
    """The mock can impersonate any of the three modes, so the server's ``ready.mode`` plumbing
    and any mode-dependent reporting can be tested without three real runtimes."""

    chunk_ms: int = 160
    chunk_ms_configurable: bool = True
    supports_context: bool = False
    """When true, terms passed to ``set_context`` are **protected from perturbation** — see
    ``set_context``. This makes plan.md §4's context-enabled vs context-free comparison
    exercisable end to end rather than merely declarable."""

    # ── How fast it reveals ─────────────────────────────────────────────────────────────────
    emit_every_ms: int = 240
    """Milliseconds of *speech* between hypotheses. Silence does not count toward it."""

    words_per_emission: int = 2
    words_per_emission_jitter: int = 1
    """Reveal ``words_per_emission ± jitter`` words per emission, minimum 1."""

    # ── How it revises ──────────────────────────────────────────────────────────────────────
    revision_depth: int = 2
    """How many trailing words are shown in a corrupted form and corrected on the next emission.
    This is the ordinary tail churn plan.md §1.4 step 3 retracts ``M`` clusters to avoid."""

    deep_revision_prob: float = 0.15
    deep_revision_depth: int = 4
    """How often, and how far, a revision reaches back further than usual.

    **Whether a given depth violates the invariant is not the mock's to know.** Eligibility is
    decided by the server's stability parameters (plan.md §4: ``N∈{2,3} × M∈{2,3,5} ×
    T∈{100,200,400}ms``), and a deep revision that is harmless at ``M=5`` may be inadmissible at
    ``M=2``. The defaults here are tuned to be survivable at the plan's defaults (N=2, M=3) while
    still exercising retraction; a sweep that turns stability down should expect ICER > 0 from
    this backend and that is a correct measurement of the sweep, not a mock bug. Use
    ``hostile_stability`` when a *guaranteed* violation is wanted."""

    # ── Latency injection ───────────────────────────────────────────────────────────────────
    latency_ms: int = 0
    latency_jitter_ms: int = 0
    """Artificial per-chunk delay, ``latency_ms ± jitter``. Applied through ``sleep_fn`` so a
    test can inject a recorder and assert on the requested delay without actually waiting."""

    sleep_fn: Callable[[float], None] = time.sleep

    # ── Silence ─────────────────────────────────────────────────────────────────────────────
    silence_behaviour: SilenceBehaviour = SilenceBehaviour.QUIET
    silence_peak_threshold: int = 500
    """Peak |sample| at or below which a chunk counts as silence. 500/32768 ≈ -36 dBFS."""

    hallucinate_after_ms: int = 800
    """Contiguous silence before the first hallucination. Deliberately near the 0.8 s endpoint
    silence of plan.md §1.6, so a correctly gated pipeline never reaches it and an ungated one
    reaches it immediately."""

    hallucinate_every_ms: int = 1500
    hallucinations: tuple[str, ...] = ("Thank you.", "Bye.", "Thanks for watching!")

    # ── Fault injection ─────────────────────────────────────────────────────────────────────
    hostile_stability: HostileMode = HostileMode.OFF
    """**Hostile. Off by default.** See ``HostileMode``."""

    hostile_after_emissions: int = 2
    """Emissions of well-behaved output before the violation starts, so the client has published
    something real to be retracted."""

    decode_failure_after_ms: Optional[int] = None
    """Raise ``BackendDecodeError`` once this much audio has been fed, to exercise the
    ``decode_failed`` (non-fatal, session-aborting) path of docs/protocol.md §11.3."""

    load_ms: int = 0
    warmup_ms: int = 0
    """Artificial load / warm-up cost, so the server's ``loading`` heartbeat and the
    ``READY_TIMEOUT_MS`` restart rule have something slow to relay."""

    seed: int = 0

    notes: str = ""

    def __post_init__(self) -> None:
        # `--backend-opt KEY=VALUE` delivers strings, and `connection._coerce_option` can turn one
        # into an int, a float or a bool but not into an enum member — only the backend knows
        # which of its fields is an enum. Coercing here is what makes `--backend-opt
        # hostile_stability=decrease` work from a models.json entry; without it the string
        # survives all the way to `capabilities`, where `.value` fails with an AttributeError
        # about `str` that says nothing about the command line that caused it.
        if isinstance(self.hostile_stability, str):
            try:
                self.hostile_stability = HostileMode(self.hostile_stability)
            except ValueError:
                raise ValueError(
                    f"hostile_stability must be one of "
                    f"{', '.join(m.value for m in HostileMode)}; got "
                    f"{self.hostile_stability!r}"
                ) from None
        if isinstance(self.silence_behaviour, str):
            try:
                self.silence_behaviour = SilenceBehaviour(self.silence_behaviour)
            except ValueError:
                raise ValueError(
                    f"silence_behaviour must be one of "
                    f"{', '.join(m.value for m in SilenceBehaviour)}; got "
                    f"{self.silence_behaviour!r}"
                ) from None

        if self.emit_every_ms <= 0:
            raise ValueError("emit_every_ms must be positive")
        if self.words_per_emission <= 0:
            raise ValueError("words_per_emission must be positive")
        if self.words_per_emission_jitter < 0:
            raise ValueError("words_per_emission_jitter must be non-negative")
        if not 0.0 <= self.deep_revision_prob <= 1.0:
            raise ValueError("deep_revision_prob must be in [0, 1]")
        if self.silence_behaviour is SilenceBehaviour.HALLUCINATE and not self.hallucinations:
            raise ValueError("hallucinate mode needs at least one hallucination phrase")


# ─── The backend ────────────────────────────────────────────────────────────────────────────


class MockBackend(Backend):
    """Deterministic scripted transcription with realistic revision behaviour."""

    def __init__(self, config: Optional[MockConfig] = None, **overrides) -> None:
        super().__init__()
        cfg = config or MockConfig()
        for k, v in overrides.items():
            if not hasattr(cfg, k):
                raise TypeError(f"MockConfig has no field {k!r}")
            setattr(cfg, k, v)
        cfg.__post_init__()
        self._cfg = cfg

        self._script: tuple[str, ...] = ()
        self._utterance_index = 0

        # Per-utterance state; see _begin_utterance.
        self._rng = random.Random(cfg.seed)
        self._words: list[str] = []
        self._revealed = 0
        self._emissions = 0
        self._tail_clean = True
        self._started = False
        self._hostile_mark: Optional[int] = None

        # Per-session state.
        self._audio_ms = 0
        self._pending_ms = 0
        self._silence_ms = 0
        self._since_hallucination_ms = 0
        self._hallucinated = ""
        self._protected: frozenset[str] = frozenset()
        self._failed = False

    # ── Declaration ─────────────────────────────────────────────────────────────────────────

    @property
    def capabilities(self) -> BackendCapabilities:
        cfg = self._cfg
        note = cfg.notes or "scripted mock backend; no model, no dependencies"
        if cfg.hostile_stability is not HostileMode.OFF:
            note = f"HOSTILE({cfg.hostile_stability.value}) — {note}"
        return BackendCapabilities(
            model_id=cfg.model_id,
            mode=cfg.mode,
            supports_context=cfg.supports_context,
            natural_chunk_ms=cfg.chunk_ms,
            chunk_ms_configurable=cfg.chunk_ms_configurable,
            supported_chunk_ms=(),
            # False on purpose: the mock's "decoder state" is an index into a word list, so a gap
            # in the audio costs it nothing. Declaring True would make on_discontinuity() reset
            # the utterance and mask whether the *server* handles discontinuity correctly.
            requires_contiguous_audio=False,
            notes=note,
        )

    @property
    def config(self) -> MockConfig:
        return self._cfg

    @property
    def script(self) -> tuple[str, ...]:
        return self._script

    # ── Lifecycle ───────────────────────────────────────────────────────────────────────────

    def _load(self, progress: Optional[ProgressFn]) -> None:
        self._report(progress, "script", 30)
        self._script = _resolve_script(self._cfg)
        if not self._script:
            raise BackendLoadError("mock script is empty")
        if self._cfg.load_ms > 0:
            self._cfg.sleep_fn(self._cfg.load_ms / 1000.0)
        self._report(progress, "ready", 90)

    def _warm_up(self, audio_ms: int) -> None:
        # The base implementation feeds silence and finalizes, which would advance this backend's
        # position in the script — so a warmed server would start the user's first real session on
        # utterance 2. Save and restore. (A real backend has no such counter; this is the price of
        # a mock whose output is positional.)
        saved = self._utterance_index
        if self._cfg.warmup_ms > 0:
            self._cfg.sleep_fn(self._cfg.warmup_ms / 1000.0)
        super()._warm_up(audio_ms)
        self._utterance_index = saved

    # ── Context ─────────────────────────────────────────────────────────────────────────────

    def set_context(self, terms: Sequence[str]) -> bool:
        """Accept vocabulary hints; protect those terms from perturbation when supported.

        This is the mock's model of biasing, and it is deliberately optimistic: a real biased
        decoder raises the *probability* of a hinted term, it does not guarantee it. What matters
        for the harness is that context-enabled and context-free runs differ **measurably and
        in the expected direction** — otherwise a bug in the ``start.context`` plumbing (terms
        never reaching the backend, or reaching it in the wrong session) would look exactly like
        "biasing did not help much", which is a result plan.md §4 might plausibly accept.

        When ``supports_context`` is false the terms are stored and ignored, and this returns
        False, matching the docs/protocol.md §6.1 requirement to report ``context_supported:false``
        rather than silently pretending.
        """
        applied = super().set_context(terms)
        self._protected = (
            frozenset(t.casefold() for t in terms if t) if applied else frozenset()
        )
        return applied

    # ── Streaming ───────────────────────────────────────────────────────────────────────────

    def feed(self, pcm: bytes) -> list[Hypothesis]:
        self._require_loaded("feed")
        self._check_pcm(pcm)
        if not pcm:
            return []

        ms = bytes_to_ms(len(pcm))
        self._audio_ms += ms
        self._inject_latency()

        cfg = self._cfg
        if (
            cfg.decode_failure_after_ms is not None
            and not self._failed
            and self._audio_ms >= cfg.decode_failure_after_ms
        ):
            self._failed = True
            raise BackendDecodeError(
                f"injected decode failure after {self._audio_ms} ms of audio "
                f"(MockConfig.decode_failure_after_ms)"
            )

        if pcm_peak(pcm) <= cfg.silence_peak_threshold:
            return self._feed_silence(ms)

        self._silence_ms = 0
        self._since_hallucination_ms = 0
        self._pending_ms += ms
        out: list[Hypothesis] = []
        while self._pending_ms >= cfg.emit_every_ms:
            self._pending_ms -= cfg.emit_every_ms
            h = self._emit()
            if h is None:
                # Nothing left to reveal for this utterance. Stop draining rather than spinning:
                # a real decoder that has converged emits nothing either.
                self._pending_ms = 0
                break
            out.append(h)
        return out

    def _feed_silence(self, ms: int) -> list[Hypothesis]:
        cfg = self._cfg
        self._silence_ms += ms
        if cfg.silence_behaviour is not SilenceBehaviour.HALLUCINATE:
            return []
        if self._silence_ms < cfg.hallucinate_after_ms:
            return []
        self._since_hallucination_ms += ms
        first = not self._hallucinated
        if not first and self._since_hallucination_ms < cfg.hallucinate_every_ms:
            return []
        self._since_hallucination_ms = 0
        if not self._started:
            # Silence-only utterance: there is no real text to append to, so the hallucination is
            # the entire hypothesis. This is the case plan.md §4 measures with silence-only clips.
            self._begin_utterance()
        phrase = self._rng.choice(cfg.hallucinations)
        self._hallucinated = _join(self._hallucinated, phrase)
        self._emissions += 1
        return [
            Hypothesis(
                text=self._current_text(),
                audio_pos_ms=self._audio_ms,
            )
        ]

    def finalize(self) -> Optional[Hypothesis]:
        self._require_loaded("finalize")
        if not self._started:
            return None
        # A real decoder flushes: everything it heard, cleanly, whether or not it had got around
        # to revealing it. Revealing the whole utterance here is also what makes the final
        # genuinely differ from the last partial, which is what a commit engine must handle.
        self._revealed = len(self._words)
        self._tail_clean = True
        self._emissions += 1
        text = self._current_text()
        text, override, hostile = self._hostile_fields(text)
        h = Hypothesis(
            text=text,
            is_final=True,
            audio_pos_ms=self._audio_ms,
            stable_bytes_override=override,
            hostile=hostile,
        )
        self._end_utterance()
        return h

    def _reset(self, reason: ResetReason) -> None:
        self._end_utterance()
        self._pending_ms = 0
        self._silence_ms = 0
        self._since_hallucination_ms = 0
        if reason in (ResetReason.SESSION_START, ResetReason.SESSION_END, ResetReason.WARMUP):
            self._audio_ms = 0
            self._failed = False
            if reason is not ResetReason.WARMUP:
                self._utterance_index = 0

    # ── Emission ────────────────────────────────────────────────────────────────────────────

    def _emit(self) -> Optional[Hypothesis]:
        if not self._started:
            self._begin_utterance()
        cfg = self._cfg
        n = len(self._words)
        if self._revealed >= n and self._tail_clean:
            return None

        if self._revealed < n:
            jitter = cfg.words_per_emission_jitter
            step = cfg.words_per_emission + (
                self._rng.randint(-jitter, jitter) if jitter else 0
            )
            self._revealed = min(n, self._revealed + max(1, step))

        depth = (
            cfg.deep_revision_depth
            if self._rng.random() < cfg.deep_revision_prob
            else cfg.revision_depth
        )
        if self._revealed >= n:
            # Everything is revealed; this emission cleans the tail and the next returns None.
            # Without this the mock would perturb forever and never converge.
            depth = 0
        depth = min(depth, self._revealed)

        words = list(self._words[: self._revealed])
        changed = False
        for i in range(self._revealed - depth, self._revealed):
            if self._is_protected(words[i]):
                continue
            perturbed = _perturb(words[i], self._rng)
            if perturbed != words[i]:
                words[i] = perturbed
                changed = True
        self._tail_clean = not changed

        self._emissions += 1
        text = self._current_text(words)
        text, override, hostile = self._hostile_fields(text)
        return Hypothesis(
            text=text,
            audio_pos_ms=self._audio_ms,
            stable_bytes_override=override,
            hostile=hostile,
        )

    def _current_text(self, words: Optional[Sequence[str]] = None) -> str:
        if words is None:
            words = self._words[: self._revealed]
        # Hallucination trails the real text, the way Whisper appends "Thank you." past the end of
        # the audio (plan.md §1.6).
        return _join(" ".join(words), self._hallucinated)

    def _is_protected(self, word: str) -> bool:
        if not self._protected:
            return False
        return word.casefold().strip(".,;:!?") in self._protected or word.casefold() in self._protected

    # ── Utterance bookkeeping ───────────────────────────────────────────────────────────────

    def _begin_utterance(self) -> None:
        cfg = self._cfg
        idx = self._utterance_index
        if idx >= len(self._script):
            self._words = [] if not cfg.loop_script else list(
                self._script[idx % len(self._script)].split()
            )
        else:
            self._words = list(self._script[idx].split())
        # Reseed per utterance so utterance n is reproducible independently of what ran before it.
        self._rng = random.Random(cfg.seed * 1_000_003 + idx)
        self._revealed = 0
        self._emissions = 0
        self._tail_clean = True
        self._hostile_mark = None
        self._hallucinated = ""
        self._started = True

    def _end_utterance(self) -> None:
        if not self._started:
            return
        self._started = False
        self._utterance_index += 1
        self._words = []
        self._revealed = 0
        self._emissions = 0
        self._tail_clean = True
        self._hostile_mark = None
        self._hallucinated = ""

    # ── Latency ─────────────────────────────────────────────────────────────────────────────

    def _inject_latency(self) -> None:
        cfg = self._cfg
        if cfg.latency_ms <= 0 and cfg.latency_jitter_ms <= 0:
            return
        jitter = cfg.latency_jitter_ms
        delay = cfg.latency_ms + (self._rng.randint(-jitter, jitter) if jitter else 0)
        if delay > 0:
            cfg.sleep_fn(delay / 1000.0)

    # ── Hostile modes ───────────────────────────────────────────────────────────────────────

    def _hostile_fields(self, text: str) -> tuple[str, Optional[int], bool]:
        """Return ``(text, stable_bytes_override, hostile)``.

        Every offset here is produced by ``_word_end_offsets``, which counts **UTF-8 bytes**, not
        code points. That is not incidental: the whole point of a hostile mode is to hand the
        client an offset it must reject for the *stated* reason, and an offset that was
        accidentally a code-point index would be rejected for the wrong one — the test would pass
        while proving nothing.
        """
        cfg = self._cfg
        mode = cfg.hostile_stability
        if mode is HostileMode.OFF or self._emissions <= cfg.hostile_after_emissions:
            return text, None, False

        total = utf8_len(text)
        bounds = _word_end_offsets(text)

        if mode is HostileMode.DECREASE:
            if self._hostile_mark is None:
                mark = _pick_boundary(bounds, total, 0.6)
            else:
                lower = [b for b in bounds if b < self._hostile_mark]
                mark = lower[-1] if lower else 0
            self._hostile_mark = mark
            return text, mark, True

        # MUTATE: hold the offset still and change the bytes underneath it.
        if self._hostile_mark is None:
            mark = _pick_boundary(bounds, total, 0.25)
            if mark <= 0:
                # Not enough text yet for a meaningful stable prefix; stay well-behaved and try
                # again next emission rather than emitting a degenerate offset of 0.
                return text, None, False
            self._hostile_mark = mark
        mark = min(self._hostile_mark, total)
        if self._emissions % 2 == 0:
            # Byte-length-preserving, so `mark` still lands exactly where it did.
            text = _flip_first_ascii(text)
        return text, mark, True


# ─── Script loading ─────────────────────────────────────────────────────────────────────────


def _resolve_script(cfg: MockConfig) -> tuple[str, ...]:
    if cfg.script_path is not None:
        path = Path(cfg.script_path)
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError as exc:
            raise BackendLoadError(f"cannot read mock script {path}: {exc}") from exc
        except UnicodeDecodeError as exc:
            # Explicit rather than falling back to a lenient decode: a script file that is not
            # UTF-8 would inject replacement characters into text destined for a keystroke
            # injector, which docs/protocol.md §4.1 forbids on the wire for the same reason.
            raise BackendLoadError(f"mock script {path} is not valid UTF-8: {exc}") from exc
        lines = raw.splitlines()
    elif cfg.script is None:
        return DEFAULT_SCRIPT
    elif isinstance(cfg.script, str):
        lines = cfg.script.splitlines()
    else:
        lines = list(cfg.script)
    out = [ln.strip() for ln in lines]
    return tuple(ln for ln in out if ln and not ln.startswith("#"))


# ─── Perturbation: the mis-transcriptions this product exists to fix ────────────────────────


def _drop_final_punct(w: str) -> str:
    return w.rstrip(".,;:!?")


def _lower_initial(w: str) -> str:
    return w[:1].lower() + w[1:] if w[:1].isupper() else w


def _spell_out_dot(w: str) -> str:
    """``llama.cpp`` -> ``llama dot cpp``.

    Named in plan.md §4 as the exact error that normalised WER hides and that the technical-term
    F1 metric exists to catch. A mock that never produces it would let a harness measuring that
    metric report a perfect score against a backend that cannot spell an identifier."""
    return w.replace(".", " dot ").replace("_", " underscore ").strip() if ("." in w or "_" in w) else w


def _spell_out_slash(w: str) -> str:
    return w.replace("/", " slash ").strip() if "/" in w else w


_HOMOPHONES = {
    "to": "two",
    "two": "to",
    "the": "a",
    "and": "an",
    "use": "juice",
    "check": "cheque",
    "whether": "weather",
    "we": "wii",
    "for": "four",
    "instead": "in stead",
    "default": "de fault",
    "config": "conflict",
}


def _homophone(w: str) -> str:
    return _HOMOPHONES.get(w.casefold(), w)


def _truncate(w: str) -> str:
    """The head of a word the decoder has not finished hearing. Always changes a word of length
    >= 2, which is what makes ``_perturb`` able to guarantee a revision."""
    return w[:-1] if len(w) >= 2 else w


_PERTURBATIONS = (
    _spell_out_dot,
    _spell_out_slash,
    _homophone,
    _drop_final_punct,
    _lower_initial,
    _truncate,
)


def _perturb(word: str, rng: random.Random) -> str:
    """Corrupt one word deterministically. Tries transforms in a seeded random order and takes
    the first that actually changes the word, so a revision is produced whenever one is possible
    rather than silently skipped when the randomly-chosen transform happens not to apply."""
    order = list(_PERTURBATIONS)
    rng.shuffle(order)
    for fn in order:
        out = fn(word)
        if out and out != word:
            return out
    return word


# ─── Byte-offset helpers. Every offset here is UTF-8 bytes. ─────────────────────────────────


def _word_end_offsets(text: str) -> list[int]:
    """UTF-8 byte offsets at the end of each word (i.e. *before* the following whitespace).

    Word ends rather than word starts because docs/protocol.md §8.2 says trailing whitespace
    SHOULD be excluded from ``stable_bytes``: holding the space back lets a following punctuation
    revision land without rewriting across committed text (plan.md §1.4).

    These offsets are grapheme-cluster boundaries for any text that does not begin a cluster with
    a combining mark — which no real transcript does, and which the default script does not. This
    helper does not implement UAX #29 and does not pretend to; the grapheme tables are a shared,
    version-pinned asset (docs/protocol.md §8.4 and §14 item 2) and are not this module's to own.
    """
    offsets: list[int] = []
    b = 0
    prev_was_space = True
    for ch in text:
        if ch.isspace():
            if not prev_was_space and b > 0:
                offsets.append(b)
            prev_was_space = True
        else:
            prev_was_space = False
        b += len(ch.encode("utf-8"))
    if not prev_was_space and b > 0:
        offsets.append(b)
    return offsets


def _pick_boundary(bounds: Sequence[int], total: int, fraction: float) -> int:
    """The largest word-end offset at or below ``fraction`` of the text, or 0 if there is none."""
    target = int(total * fraction)
    candidates = [b for b in bounds if 0 < b <= target]
    return candidates[-1] if candidates else 0


def _flip_first_ascii(text: str) -> str:
    """Change the first ASCII character without changing the string's byte length.

    Length preservation is the whole trick of ``HostileMode.MUTATE``: it lets the offset stay
    numerically identical while the bytes below it differ, which is the failure a receiver that
    only compares the numbers will miss.
    """
    for i, ch in enumerate(text):
        if ch.isascii() and ch.isalpha():
            return text[:i] + (ch.lower() if ch.isupper() else ch.upper()) + text[i + 1 :]
    for i, ch in enumerate(text):
        if ch.isascii() and not ch.isspace():
            return text[:i] + ("#" if ch != "#" else "@") + text[i + 1 :]
    return text


def _join(a: str, b: str) -> str:
    a = a.strip()
    b = b.strip()
    if not a:
        return b
    if not b:
        return a
    return f"{a} {b}"
