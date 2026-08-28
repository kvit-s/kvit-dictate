# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Server-side stability computation: cumulative hypotheses in, ``stable_bytes`` out.

Implements plan.md §1.4 (the six-step stability algorithm plus step 7's utterance reset) and
docs/protocol.md §8.2 (``stable_bytes`` is a UTF-8 byte prefix length of the *decoded* string,
landing on an extended-grapheme-cluster boundary, monotonically non-decreasing within an
utterance, with the bytes below the mark never changing).

This module is the Python twin of ``src/commit_engine.h`` + ``src/util/utf8.h``. **The two
implementations must agree**: a disagreement is an interop bug, not a stylistic difference. The
C++ side is currently a stub (``src/commit_engine.cpp`` and ``src/util/utf8.cpp`` are TODO), so
this file is written against the *headers*, which are the contract, and every place where the
headers left a choice open is called out in a comment rather than decided silently.

Zero third-party dependencies, by house rule: stdlib only, and no import of anything a backend
needs. This module must import on a bare CPython 3.12.

─── The one bug this file exists to prevent ────────────────────────────────────────────────────

The server must NEVER emit a ``stable_bytes`` that decreases, or one whose prefix bytes differ
from what it already sent. docs/protocol.md §8.3 makes that a ``stability_violation`` that aborts
the session on the Client side — and the Client's required response is to *preserve the document*
and never repair, which means the wrong bytes stay in the user's file forever. Catching it here
costs one comparison; catching it on the Client costs a corrupted document. So every emission
goes through :meth:`StabilityTracker.check_emission`, which raises rather than returns.

Note that a raise is the only honest option once it happens: §8.2's monotonicity rule constrains
the new hypothesis against the *previously published* mark, so if the incoming text contradicts
that mark there is no value of ``stable_bytes`` that makes the message conforming. The message
cannot be sent at all. Clamping is explicitly forbidden (§8.3) because clamping hides the defect
ICER exists to measure.

─── Python strings are code points; the wire is bytes ──────────────────────────────────────────

Every offset that crosses the wire is a UTF-8 **byte** offset. Python strings are sequences of
code points. That mismatch is the single most likely bug on this side, so the internals here work
on a precomputed table of (code-point index → byte offset) built once per string, and every
public function takes and returns byte offsets. There is no place in this module where a code
point index is allowed to escape.

─── The grapheme approximation, declared up front ──────────────────────────────────────────────

``src/util/utf8.h`` declares a deliberate, documented approximation of UAX #29 and states the
exact list of things it refuses to split. This module implements *that same list*:

  * a UTF-8 multi-byte sequence;
  * a run of combining marks (Grapheme_Extend=Yes, plus Grapheme_Cluster_Break=SpacingMark);
  * a ZWJ emoji sequence (GB11);
  * a regional-indicator pair (GB12/GB13);
  * a variation selector (U+FE00–U+FE0F, U+E0100–U+E01EF) or U+20E3 combining enclosing keycap;
  * a CR LF pair (GB3).

and it gets the same things wrong, in the same conservative direction (reporting *more* clusters
than UAX #29 would, which can only delay a commit, never split text):

  * Hangul syllable composition (GB6–GB8) is not modelled;
  * Indic conjunct clusters (GB9c) are not modelled;
  * ``Extended_Pictographic`` is approximated by code-point ranges rather than a generated table,
    so GB11 applies slightly more broadly than UAX #29 specifies — it can merge two adjacent
    emoji into one cluster, never split one;
  * GCB=Prepend (e.g. U+0600) is not modelled.

**Unverified, and recorded rather than guessed** (docs/protocol.md §14 item 2, and the same open
question in ``src/util/utf8.h``): which Unicode version's property tables both endpoints pin.
This module derives Grapheme_Extend from ``unicodedata.category`` (Mn/Me) plus explicit ranges,
so it tracks *the CPython build's* Unicode version, which is not under our control and will drift
from a hand-maintained C++ table. :data:`UNICODE_VERSION` reports what this interpreter actually
has, so the drift is visible instead of silent. The White_Space set below is hard-coded rather
than taken from ``str.isspace()`` precisely so that at least the word-boundary rule is
version-stable — ``str.isspace()`` is true for U+001C–U+001F, which are not White_Space=Yes, and
treating them as word separators would widen the committed prefix, which is the unsafe direction.
"""

from __future__ import annotations

import time
import unicodedata
from bisect import bisect_right
from collections import deque
from dataclasses import dataclass
from functools import lru_cache
from typing import Callable, Sequence

__all__ = [
    "UNICODE_VERSION",
    "DEFAULT_HYPOTHESES_N",
    "DEFAULT_RETRACT_CLUSTERS_M",
    "DEFAULT_HOLD_MS_T",
    "StabilityParams",
    "StabilityTracker",
    "StabilityViolation",
    "ViolationKind",
    "ManualClock",
    "monotonic_ms",
    "utf8_len",
    "grapheme_boundaries",
    "is_grapheme_boundary",
    "floor_grapheme",
    "count_graphemes",
    "common_grapheme_prefix",
    "retract_graphemes",
    "retract_to_word_boundary",
    "trim_trailing_whitespace",
    "is_whitespace",
    "is_word_separator",
]

# The Unicode version this interpreter's tables were built against. Must match the version
# recorded in ``src/util/utf8.h`` (``kUnicodeVersion``), which currently says
# "unspecified-pending-protocol-md-14.2" — i.e. the question is open on both sides.
UNICODE_VERSION = unicodedata.unidata_version


# ─── Clock (mirrors src/util/clock.h) ───────────────────────────────────────────────────────────
#
# Injected, monotonic, milliseconds. Step 5 of the algorithm is a timer, and a timer that can only
# be tested by sleeping is a timer that will not be tested.

Clock = Callable[[], int]


def monotonic_ms() -> int:
    """Milliseconds from an unspecified monotonic epoch. Only differences are meaningful."""
    return time.monotonic_ns() // 1_000_000


class ManualClock:
    """Test clock. Time moves only when the test moves it. Mirrors ``kd::ManualClock``."""

    __slots__ = ("_now",)

    def __init__(self, start: int = 0) -> None:
        self._now = int(start)

    def __call__(self) -> int:
        return self._now

    def advance(self, delta_ms: int) -> None:
        self._now += int(delta_ms)

    def set(self, now_ms: int) -> None:
        self._now = int(now_ms)


# ─── Grapheme-cluster segmentation ──────────────────────────────────────────────────────────────

_GCB_OTHER = 0
_GCB_CR = 1
_GCB_LF = 2
_GCB_CONTROL = 3
_GCB_EXTEND = 4
_GCB_ZWJ = 5
_GCB_RI = 6
_GCB_SPACINGMARK = 7

_CR = 0x0D
_LF = 0x0A
_ZWJ = 0x200D

# Code points that are Grapheme_Extend=Yes but are *not* general category Mn/Me, so
# ``unicodedata.category`` alone would miss them. Kept as an explicit table because these are
# exactly the cases fixture 013 pins, and because the C++ side must carry the identical list.
_EXTEND_RANGES: tuple[tuple[int, int], ...] = (
    (0x200C, 0x200C),      # ZWNJ
    (0xFE00, 0xFE0F),      # variation selectors 1-16 (Mn today, listed for stability)
    (0x1F3FB, 0x1F3FF),    # emoji skin-tone modifiers (category Sk, Grapheme_Extend=Yes)
    (0xE0020, 0xE007F),    # tag characters (category Cf, Grapheme_Extend=Yes)
    (0xE0100, 0xE01EF),    # variation selectors 17-256
)

# Extended_Pictographic, approximated by ranges. Deliberately broad; see the module docstring for
# why the error direction is safe. This is the table that must be generated into both languages
# from one source once docs/protocol.md §14 item 2 is answered.
_EXTPICT_RANGES: tuple[tuple[int, int], ...] = (
    (0x00A9, 0x00A9), (0x00AE, 0x00AE),
    (0x203C, 0x203C), (0x2049, 0x2049),
    (0x2122, 0x2122), (0x2139, 0x2139),
    (0x2194, 0x21AA), (0x231A, 0x231B), (0x2328, 0x2328),
    (0x23CF, 0x23CF), (0x23E9, 0x23F3), (0x23F8, 0x23FA),
    (0x24C2, 0x24C2), (0x25AA, 0x25AB), (0x25B6, 0x25B6), (0x25C0, 0x25C0),
    (0x25FB, 0x25FE), (0x2600, 0x27BF),
    (0x2934, 0x2935), (0x2B00, 0x2BFF),
    (0x3030, 0x3030), (0x303D, 0x303D), (0x3297, 0x3297), (0x3299, 0x3299),
    (0x1F000, 0x1FAFF), (0x1FC00, 0x1FFFD),
)


def _in_ranges(cp: int, ranges: tuple[tuple[int, int], ...]) -> bool:
    for lo, hi in ranges:
        if lo <= cp <= hi:
            return True
        if cp < lo:
            return False
    return False


@lru_cache(maxsize=8192)
def _is_extpict(cp: int) -> bool:
    return _in_ranges(cp, _EXTPICT_RANGES)


@lru_cache(maxsize=8192)
def _gcb(cp: int) -> int:
    """Grapheme_Cluster_Break class, in the approximation this project ships."""
    if cp == _CR:
        return _GCB_CR
    if cp == _LF:
        return _GCB_LF
    if cp == _ZWJ:
        return _GCB_ZWJ
    if 0x1F1E6 <= cp <= 0x1F1FF:
        return _GCB_RI
    if _in_ranges(cp, _EXTEND_RANGES):
        return _GCB_EXTEND
    cat = unicodedata.category(chr(cp))
    if cat in ("Mn", "Me"):
        return _GCB_EXTEND
    if cat == "Mc":
        # Approximation: GCB=SpacingMark is Mc minus a documented exception list. Treating the
        # exceptions as SpacingMark merges where UAX #29 would break, i.e. fewer boundaries, i.e.
        # a narrower committed prefix. Safe direction.
        return _GCB_SPACINGMARK
    if cat in ("Cc", "Cf", "Zl", "Zp"):
        return _GCB_CONTROL
    return _GCB_OTHER


def _utf8_size(cp: int) -> int:
    if cp < 0x80:
        return 1
    if cp < 0x800:
        return 2
    if cp < 0x10000:
        return 3
    return 4


class _Analysis:
    """Everything derived from one string: byte offsets per code point, and cluster boundaries.

    Built once and cached, because a single :meth:`StabilityTracker.observe` call asks for cluster
    boundaries of the same text several times over.
    """

    __slots__ = ("byte_at", "cluster_cp", "cluster_bytes", "cluster_byte_set", "nbytes")

    def __init__(self, text: str) -> None:
        n = len(text)
        byte_at = [0] * (n + 1)
        b = 0
        for i, ch in enumerate(text):
            b += _utf8_size(ord(ch))
            byte_at[i + 1] = b
        self.byte_at = byte_at
        self.nbytes = b

        bounds = _cluster_cp_boundaries(text)
        self.cluster_cp = bounds
        self.cluster_bytes = tuple(byte_at[i] for i in bounds)
        self.cluster_byte_set = frozenset(self.cluster_bytes)


def _cluster_cp_boundaries(text: str) -> tuple[int, ...]:
    """Code-point indices of every cluster boundary, including 0 and ``len(text)``."""
    n = len(text)
    if n == 0:
        return (0,)

    bounds = [0]
    cps = [ord(c) for c in text]

    # State for GB11 (ExtPict Extend* ZWJ × ExtPict):
    #   0 = not in a pictographic chain
    #   1 = ExtPict seen, possibly followed by Extends
    #   2 = ExtPict Extend* ZWJ seen
    def advance(state: int, cp: int, cls: int) -> int:
        if _is_extpict(cp):
            return 1
        if state == 1 and cls == _GCB_EXTEND:
            return 1
        if state == 1 and cls == _GCB_ZWJ:
            return 2
        return 0

    g_prev = _gcb(cps[0])
    pict_state = advance(0, cps[0], g_prev)
    ri_run = 1 if g_prev == _GCB_RI else 0

    for i in range(1, n):
        cp = cps[i]
        g_cur = _gcb(cp)

        if g_prev == _GCB_CR and g_cur == _GCB_LF:
            brk = False                                     # GB3
        elif g_prev in (_GCB_CR, _GCB_LF, _GCB_CONTROL):
            brk = True                                      # GB4
        elif g_cur in (_GCB_CR, _GCB_LF, _GCB_CONTROL):
            brk = True                                      # GB5
        elif g_cur in (_GCB_EXTEND, _GCB_ZWJ):
            brk = False                                     # GB9
        elif g_cur == _GCB_SPACINGMARK:
            brk = False                                     # GB9a
        elif pict_state == 2 and _is_extpict(cp):
            brk = False                                     # GB11
        elif g_prev == _GCB_RI and g_cur == _GCB_RI and (ri_run % 2) == 1:
            brk = False                                     # GB12 / GB13
        else:
            brk = True                                      # GB999

        if brk:
            bounds.append(i)
            pict_state = advance(0, cp, g_cur)
            ri_run = 1 if g_cur == _GCB_RI else 0
        else:
            pict_state = advance(pict_state, cp, g_cur)
            if g_cur == _GCB_RI:
                ri_run = ri_run + 1 if g_prev == _GCB_RI else 1
            else:
                ri_run = 0

        g_prev = g_cur

    bounds.append(n)
    return tuple(bounds)


@lru_cache(maxsize=256)
def _analyse(text: str) -> _Analysis:
    return _Analysis(text)


# ─── Public UTF-8 / grapheme helpers ────────────────────────────────────────────────────────────
#
# These are the Python counterparts of ``kd::utf8`` (src/util/utf8.h). They live here rather than
# in a module of their own only because of file ownership during this phase; if a shared
# ``kvit_asr.utf8`` module is created, it should *move* these rather than reimplement them —
# two Python grapheme tables in one process is the same interop bug as two across languages.


def utf8_len(text: str) -> int:
    """Length of ``text`` in UTF-8 bytes. The unit every wire offset is measured in."""
    return _analyse(text).nbytes


def grapheme_boundaries(text: str) -> tuple[int, ...]:
    """UTF-8 byte offsets of every grapheme-cluster boundary, including 0 and ``utf8_len``."""
    return _analyse(text).cluster_bytes


def count_graphemes(text: str) -> int:
    return len(_analyse(text).cluster_bytes) - 1


def is_grapheme_boundary(text: str, byte_off: int) -> bool:
    """docs/protocol.md §8.2 requirement 2, as a predicate on a UTF-8 byte offset.

    A UTF-8 sequence boundary is necessary but *not* sufficient — fixture 013 is built entirely
    from offsets that pass a code-point check and fail this one.
    """
    a = _analyse(text)
    return 0 <= byte_off <= a.nbytes and byte_off in a.cluster_byte_set


def floor_grapheme(text: str, byte_off: int) -> int:
    """Snap ``byte_off`` down to the nearest cluster boundary at or before it.

    Never moves forward: rounding *up* would publish a byte the model has not stabilised.
    """
    a = _analyse(text)
    if byte_off <= 0:
        return 0
    if byte_off >= a.nbytes:
        return a.nbytes
    i = bisect_right(a.cluster_bytes, byte_off) - 1
    return a.cluster_bytes[i]


def common_grapheme_prefix(a: str, b: str) -> int:
    """Byte length of the longest common prefix of ``a`` and ``b``, on a cluster boundary.

    plan.md §1.4 step 2. Not a byte-wise prefix: two hypotheses that differ only in a combining
    mark share a byte prefix that ends *inside* a cluster, and publishing that offset would commit
    a base letter whose accent the model is still revising.
    """
    if a == b:
        return utf8_len(a)
    A = _analyse(a)
    B = _analyse(b)
    ka, kb = A.cluster_cp, B.cluster_cp
    matched = 0
    for k in range(1, min(len(ka), len(kb))):
        if ka[k] != kb[k]:
            break
        start = ka[k - 1]
        end = ka[k]
        if a[start:end] != b[start:end]:
            break
        matched = k
    return A.cluster_bytes[matched]


def retract_graphemes(text: str, byte_off: int, clusters: int) -> int:
    """Move ``byte_off`` back by ``clusters`` grapheme clusters, clamping at 0.

    plan.md §1.4 step 3 — "retract to at least M clusters behind the decode head", because the
    tail of a hypothesis is where revision concentrates.
    """
    if clusters <= 0:
        return floor_grapheme(text, byte_off)
    a = _analyse(text)
    off = floor_grapheme(text, byte_off)
    # floor_grapheme() guarantees `off` is itself a boundary, so this index is exact.
    i = bisect_right(a.cluster_bytes, off) - 1
    j = i - clusters
    return a.cluster_bytes[j] if j > 0 else 0


# White_Space=Yes, hard-coded. See the module docstring for why this is not ``str.isspace()``.
_WHITESPACE: frozenset[int] = frozenset(
    [0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0x85, 0xA0, 0x1680,
     0x2028, 0x2029, 0x202F, 0x205F, 0x3000]
    + list(range(0x2000, 0x200B))
)

# Terminal punctuation that reliably ends a word in dictated text, plus the fullwidth/CJK forms.
# See :func:`is_word_separator` for the positional rule that keeps "llama.cpp" intact.
_PUNCT_SEPARATORS: frozenset[int] = frozenset(
    ord(c) for c in ".,;:!?…、。！，．：；？"
)


def is_whitespace(cp: int | str) -> bool:
    """True for code points with Unicode White_Space=Yes."""
    return (ord(cp) if isinstance(cp, str) else cp) in _WHITESPACE


def is_word_separator(cp: int | str) -> bool:
    """True for code points this project treats as a word separator.

    Any whitespace, plus terminal punctuation. Note that this is a *pure* predicate on the code
    point; the positional rule that keeps ``llama.cpp`` from being committed as ``llama.`` lives
    in :func:`retract_to_word_boundary`, which requires a punctuation separator to be followed by
    whitespace or end of text. ``src/util/utf8.h`` splits the responsibility the same way.
    """
    c = ord(cp) if isinstance(cp, str) else cp
    return c in _WHITESPACE or c in _PUNCT_SEPARATORS


def trim_trailing_whitespace(text: str, byte_off: int) -> int:
    """End of ``text[0:byte_off)`` with any trailing whitespace run removed, in bytes."""
    a = _analyse(text)
    off = floor_grapheme(text, byte_off)
    # Walk back over code points; whitespace never combines, so this stays on a cluster boundary.
    i = bisect_right(a.byte_at, off) - 1
    while i > 0 and ord(text[i - 1]) in _WHITESPACE:
        i -= 1
    return a.byte_at[i]


def retract_to_word_boundary(text: str, byte_off: int) -> int:
    """plan.md §1.4 step 4, plus the trailing-whitespace hold-back.

    Returns the largest offset ``r <= byte_off`` that is a cluster boundary and ends a word, with
    any trailing whitespace then trimmed off. Never commit a partial word: a revised word ending
    is the most common revision there is. Trailing whitespace is held back so that a following
    punctuation revision does not have to rewrite across a committed space.

    The worked examples in ``src/util/utf8.h`` are the acceptance criteria, and they are also the
    reason this is not the literal reading of that header's three bullet points. Taken literally,
    "``text[0, r)`` does not end with whitespace" would reject ``r = 6`` for ``("hello world", 11)``
    and fall through to 0, but the header's own worked example says the answer is 5. The rule that
    reproduces every example is: find the word end, *then* trim. That is what this implements, and
    the discrepancy is reported rather than resolved silently.

        ("hello world", 11) -> 5     ("hello"; the space is held back)
        ("hello world",  5) -> 0     (no separator below the offset)
        ("Hi. There",    9) -> 3     ("Hi."; punctuation kept, the space held back)
        ("hello",        5) -> 0     (nothing is safe to commit yet)
        ("hello ",       6) -> 5     (the trailing space is held back)
    """
    a = _analyse(text)
    limit = floor_grapheme(text, byte_off)
    if limit <= 0:
        return 0

    nbytes = a.nbytes
    # Walk candidate cluster boundaries downward from `limit`.
    i = bisect_right(a.cluster_bytes, limit) - 1
    while i > 0:
        r = a.cluster_bytes[i]
        cp_end = a.cluster_cp[i]          # code-point index just past the candidate
        last = ord(text[cp_end - 1])
        if last in _WHITESPACE:
            return trim_trailing_whitespace(text, r)
        if last in _PUNCT_SEPARATORS:
            # A separator only counts as one when it is followed by whitespace or end of text.
            # "llama.cpp" is the motivating example in plan.md §4: treating the inner dot as a
            # word end would commit "llama." and then have to append "cpp".
            if r >= nbytes or ord(text[cp_end]) in _WHITESPACE:
                return trim_trailing_whitespace(text, r)
        i -= 1
    return 0


# ─── Violations ─────────────────────────────────────────────────────────────────────────────────


class ViolationKind:
    """Spellings match ``kd::ViolationKind`` in ``src/commit_engine.h``, in snake_case."""

    STABLE_BYTES_OUT_OF_RANGE = "stable_bytes_out_of_range"
    STABLE_BYTES_NOT_ON_BOUNDARY = "stable_bytes_not_on_boundary"
    STABLE_BYTES_DECREASED = "stable_bytes_decreased"
    STABLE_PREFIX_MUTATED = "stable_prefix_mutated"


class StabilityViolation(RuntimeError):
    """The server was about to emit a ``stable_bytes`` that docs/protocol.md §8.2 forbids.

    Raised, never returned, and never clamped. §8.3 forbids clamping because clamping hides
    exactly the defect ICER exists to measure — and in this direction (the *sender* clamping) it
    would additionally mean shipping a message whose bytes below the previously published mark
    have changed, which is the failure that corrupts the user's document.

    Carries everything §8.3 step 3 requires in the log line.
    """

    def __init__(
        self,
        kind: str,
        *,
        utterance_id: int = 0,
        previous_prefix: bytes = b"",
        incoming_text: str = "",
        previous_stable_bytes: int = 0,
        incoming_stable_bytes: int = 0,
        diverge_offset: int = 0,
    ) -> None:
        self.kind = kind
        self.utterance_id = utterance_id
        self.previous_prefix = previous_prefix
        self.incoming_text = incoming_text
        self.previous_stable_bytes = previous_stable_bytes
        self.incoming_stable_bytes = incoming_stable_bytes
        self.diverge_offset = diverge_offset
        super().__init__(
            f"{kind}: utterance={utterance_id} "
            f"previous_stable_bytes={previous_stable_bytes} "
            f"incoming_stable_bytes={incoming_stable_bytes} "
            f"diverge_offset={diverge_offset} "
            f"previous_prefix={previous_prefix!r} incoming_text={incoming_text!r}"
        )


# ─── Parameters ─────────────────────────────────────────────────────────────────────────────────

DEFAULT_HYPOTHESES_N = 2
DEFAULT_RETRACT_CLUSTERS_M = 3
DEFAULT_HOLD_MS_T = 200


@dataclass(frozen=True)
class StabilityParams:
    """The three knobs plan.md §4 sweeps: ``N ∈ {2,3} × M ∈ {2,3,5} × T ∈ {100,200,400} ms``.

    Cadence is deliberately *not* here. plan.md §1.4 separates eligibility from cadence: cadence
    can delay a flush, it can never promote unstable text, and on this side of the pipe there is
    no cadence at all — the server publishes the eligible prefix and the C++ commit engine decides
    when to type it.
    """

    hypotheses_n: int = DEFAULT_HYPOTHESES_N
    retract_clusters_m: int = DEFAULT_RETRACT_CLUSTERS_M
    hold_ms_t: int = DEFAULT_HOLD_MS_T

    def __post_init__(self) -> None:
        if self.hypotheses_n < 1:
            raise ValueError("hypotheses_n must be >= 1")
        if self.retract_clusters_m < 0:
            raise ValueError("retract_clusters_m must be >= 0")
        if self.hold_ms_t < 0:
            raise ValueError("hold_ms_t must be >= 0")


# ─── The tracker ────────────────────────────────────────────────────────────────────────────────


class StabilityTracker:
    """Turns a stream of cumulative hypotheses into a conforming ``stable_bytes`` sequence.

    One tracker per utterance-in-progress; :meth:`reset` at every utterance boundary (plan.md §1.4
    step 7, docs/protocol.md §8.2's high-water mark resets with it).

    Usage::

        t = StabilityTracker(clock=monotonic_ms)
        t.reset(utterance_id=1)
        stable = t.observe("can you change the")      # -> publish partial
        stable = t.poll()                             # -> the T-ms hold may have matured
        stable = t.finalise("Can you change the llama.cpp config")
    """

    __slots__ = (
        "_params", "_clock", "_window", "_utterance_id",
        "_high_water", "_stable_prefix", "_candidate_prefix", "_candidate_since", "_last_text",
    )

    def __init__(self, params: StabilityParams | None = None, clock: Clock = monotonic_ms) -> None:
        self._params = params or StabilityParams()
        self._clock = clock
        self._window: deque[str] = deque(maxlen=self._params.hypotheses_n)
        self._utterance_id = 0
        self._high_water = 0
        self._stable_prefix = b""
        self._candidate_prefix = b""
        self._candidate_since = 0
        self._last_text: str | None = None

    # ── Lifecycle ───────────────────────────────────────────────────────────────────────────────

    def reset(self, utterance_id: int = 0) -> None:
        """plan.md §1.4 step 7: reset entirely at an utterance boundary.

        Entirely means entirely — the hypothesis window, the high-water mark, the published
        prefix, and the dwell timer. Carrying any of it across a boundary would let a mark
        established for utterance *n* constrain utterance *n+1*, which §8.2 explicitly does not do.
        """
        self._window.clear()
        self._utterance_id = utterance_id
        self._high_water = 0
        self._stable_prefix = b""
        self._candidate_prefix = b""
        self._candidate_since = self._clock()
        self._last_text = None

    # ── Inspection ──────────────────────────────────────────────────────────────────────────────

    @property
    def params(self) -> StabilityParams:
        return self._params

    @property
    def utterance_id(self) -> int:
        return self._utterance_id

    @property
    def high_water(self) -> int:
        """The largest ``stable_bytes`` published for this utterance so far."""
        return self._high_water

    @property
    def stable_prefix(self) -> bytes:
        """The exact bytes published as stable so far. Compared, not recomputed."""
        return self._stable_prefix

    @property
    def last_text(self) -> str | None:
        return self._last_text

    # ── The last line of defence ────────────────────────────────────────────────────────────────

    def check_emission(self, text: str, stable_bytes: int) -> None:
        """Raise unless ``(text, stable_bytes)`` is a conforming successor to what we published.

        Every one of docs/protocol.md §8.2's three requirements, checked immediately before the
        value can reach the wire. Cheap here; a corrupted document at the far end otherwise.

        Public because a synthetic backwards step is worth testing directly, and because a caller
        assembling a message by another path (a backend that computes its own stability, say)
        should be able to run the same gate.
        """
        enc = text.encode("utf-8")

        # §8.2 requirement 1 — range.
        if stable_bytes < 0 or stable_bytes > len(enc):
            raise StabilityViolation(
                ViolationKind.STABLE_BYTES_OUT_OF_RANGE,
                utterance_id=self._utterance_id,
                previous_prefix=self._stable_prefix,
                incoming_text=text,
                previous_stable_bytes=self._high_water,
                incoming_stable_bytes=stable_bytes,
                diverge_offset=len(enc),
            )

        # §8.2 requirement 3, first half — monotonic.
        if stable_bytes < self._high_water:
            raise StabilityViolation(
                ViolationKind.STABLE_BYTES_DECREASED,
                utterance_id=self._utterance_id,
                previous_prefix=self._stable_prefix,
                incoming_text=text,
                previous_stable_bytes=self._high_water,
                incoming_stable_bytes=stable_bytes,
                diverge_offset=stable_bytes,
            )

        # §8.2 requirement 3, second half — the one with teeth. A non-decreasing stable_bytes
        # whose underlying bytes changed has still retracted text the Client may already have
        # typed into the user's document.
        if enc[: self._high_water] != self._stable_prefix:
            diverge = _first_difference(enc, self._stable_prefix)
            raise StabilityViolation(
                ViolationKind.STABLE_PREFIX_MUTATED,
                utterance_id=self._utterance_id,
                previous_prefix=self._stable_prefix,
                incoming_text=text,
                previous_stable_bytes=self._high_water,
                incoming_stable_bytes=stable_bytes,
                diverge_offset=diverge,
            )

        # §8.2 requirement 2 — grapheme-cluster boundary.
        if not is_grapheme_boundary(text, stable_bytes):
            raise StabilityViolation(
                ViolationKind.STABLE_BYTES_NOT_ON_BOUNDARY,
                utterance_id=self._utterance_id,
                previous_prefix=self._stable_prefix,
                incoming_text=text,
                previous_stable_bytes=self._high_water,
                incoming_stable_bytes=stable_bytes,
                diverge_offset=stable_bytes,
            )

    # ── Feeding it ──────────────────────────────────────────────────────────────────────────────

    def observe(self, text: str) -> int:
        """Take one new cumulative hypothesis; return the ``stable_bytes`` to publish with it.

        ``text`` is the complete current hypothesis for the current utterance, never a delta —
        docs/protocol.md §8.1. Passing a delta here produces silently wrong output, which is why
        the parameter is named ``text`` and not ``chunk``.

        Raises :class:`StabilityViolation` if the hypothesis contradicts what was already
        published. That is not a recoverable condition for this utterance: §8.2 constrains the new
        message against the *previously published* mark, so no value of ``stable_bytes`` makes the
        message conforming, and the message must not be sent at all.
        """
        if not isinstance(text, str):
            raise TypeError(f"hypothesis must be str, got {type(text).__name__}")

        self._window.append(text)
        self._last_text = text
        return self._evaluate(text)

    def poll(self) -> int:
        """Re-evaluate the dwell for the last hypothesis, without adding evidence.

        This is what makes step 5's T-millisecond hold fire when the backend has gone quiet: the
        candidate matured on the clock, not on a new hypothesis. Deliberately does *not* push the
        text into the window again — a repeated evaluation is not a second opinion, and counting
        it as one would make ``N`` mean "N evaluations" instead of "N hypotheses".

        **The caller must drive this on a timer, not only when it is convenient.** Step 5 is a
        clock condition and :meth:`observe` only samples the clock when a hypothesis happens to
        arrive, which has two consequences that are not obvious and that cost a full test suite
        to rediscover:

        * With ``N = 2`` the first real candidate comes into existence at the moment the second
          hypothesis lands, having then been unchanged for 0 ms. So ``observe(a); observe(b)``
          publishes nothing, *however much time passed between a and b* — the time passed before
          there was a candidate to hold. A third hypothesis, or one poll T ms later, is required.
        * Worse, for a stream whose candidate advances on every hypothesis — the normal case
          while someone is still talking — an observe-only caller restarts the dwell on each one
          and publishes **nothing at all** until the hypothesis stops moving.

        ``kvit_asr.session.Session.tick()`` is the intended driver. A tick interval of roughly
        ``T / 2`` bounds the latency this adds at about ``T / 2``.
        """
        if self._last_text is None:
            return self._high_water
        return self._evaluate(self._last_text)

    def finalise(self, text: str) -> int:
        """Close the utterance with its complete text; return ``len(utf8(text))``.

        A ``final`` is entirely stable by definition (docs/protocol.md §6.2), so the whole thing is
        the stable prefix — but §8.2's last sentence still applies: ``utf8(final.text)`` MUST begin
        with the highest stable prefix published for this utterance. Checked, not assumed.
        """
        enc = text.encode("utf-8")
        self.check_emission(text, len(enc))
        self._high_water = len(enc)
        self._stable_prefix = enc
        self._last_text = text
        return len(enc)

    # Spelling used by the C++ side is `on_final`; both names are kept so neither reviewer has to
    # translate. They are the same function.
    finalize = finalise

    # ── The algorithm ───────────────────────────────────────────────────────────────────────────

    def _evaluate(self, text: str) -> int:
        p = self._params
        now = self._clock()
        enc = text.encode("utf-8")

        # Step 0, which the plan does not number because it should never fire: the incoming
        # hypothesis must still contain everything we already published. If it does not, this
        # message cannot be sent at all.
        if enc[: self._high_water] != self._stable_prefix:
            raise StabilityViolation(
                ViolationKind.STABLE_PREFIX_MUTATED,
                utterance_id=self._utterance_id,
                previous_prefix=self._stable_prefix,
                incoming_text=text,
                previous_stable_bytes=self._high_water,
                incoming_stable_bytes=self._high_water,
                diverge_offset=_first_difference(enc, self._stable_prefix),
            )

        # Steps 1-4 produce the candidate.
        if len(self._window) < p.hypotheses_n:
            # Fewer than N hypotheses means nothing has been confirmed by a second opinion yet.
            # Publishing here would be publishing the first guess, which is the whole failure mode
            # the algorithm exists to avoid.
            candidate = 0
        else:
            # Step 2: longest common prefix over the window, on grapheme clusters.
            # LCP(S) == min over members of LCP(newest, member), because common prefixes are
            # totally ordered by length. Each pairwise result is a cluster boundary of `text`, so
            # the minimum is too.
            candidate = min(common_grapheme_prefix(text, w) for w in self._window)

            # Step 3: at least M clusters behind the decode head. The head is the end of the
            # newest hypothesis — the tail is where revision concentrates.
            #
            # Reading note: ``src/util/utf8.h`` documents retract_graphemes() as "step 3" without
            # saying whether M is subtracted from the common prefix or from the head. This takes
            # the literal reading of plan.md §1.4 ("at least M clusters behind the decode head")
            # and applies it as a cap on the candidate. Subtracting from the common prefix instead
            # would be strictly more conservative, so the two readings differ in latency, not in
            # safety — but they differ, and the C++ side must choose the same one.
            head_cap = retract_graphemes(text, len(enc), p.retract_clusters_m)
            if candidate > head_cap:
                candidate = head_cap

            # Step 4: back to the last word boundary, holding trailing whitespace back.
            candidate = retract_to_word_boundary(text, candidate)

        # Step 5: the candidate must have been unchanged for T ms.
        #
        # The dwell is tracked on the candidate *bytes*, not on the candidate length. Two different
        # prefixes of the same byte length are not the same candidate, and treating them as one
        # would let a rewrite masquerade as stability.
        candidate_prefix = enc[:candidate]
        if candidate_prefix != self._candidate_prefix:
            self._candidate_prefix = candidate_prefix
            self._candidate_since = now

        matured = (now - self._candidate_since) >= p.hold_ms_t

        # Step 6: emit as a UTF-8 byte length, monotonically non-decreasing.
        stable_bytes = self._high_water
        if matured and candidate > stable_bytes:
            stable_bytes = candidate

        self.check_emission(text, stable_bytes)

        self._high_water = stable_bytes
        self._stable_prefix = enc[:stable_bytes]
        return stable_bytes


def _first_difference(a: bytes, b: bytes) -> int:
    """Byte offset of the first difference between ``a`` and ``b`` — §8.3 step 3's log field."""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n
