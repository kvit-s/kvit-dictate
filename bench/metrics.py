"""Benchmark metrics for kvit-dictate — every number plan.md §4 gates on.

Stdlib only. No numpy, no jiwer, no third-party edit-distance library. This module is imported
by ``bench/run_matrix.py`` and by nothing in the shipping product, so it may be slow; it may not
be ambiguous.

─── What gates and what does not ──────────────────────────────────────────────────────────────

plan.md §1.4 is explicit and this module encodes it rather than leaving it to a reader:

  * **ICER gates.** Irrevocable committed error rate — committed graphemes that differ from the
    final hypothesis, per 1000 committed. It is the headline safety number because it measures
    text that is *actually wrong in the user's actual document*. Committed text is never
    repaired (plan.md §1.4), so every ICER grapheme is damage that survives the session.

  * **Flicker does not gate.** Flicker measures how much the *pending overlay* churned before
    anything was committed. A configuration can flicker heavily and corrupt nothing: the churn
    happened entirely inside our own overlay, which is what the overlay is for. Flicker is
    recorded because it is a useful diagnostic of a model's revision behaviour and because a
    reader will ask; it is not a selection criterion, and :func:`select_gate_metrics` does not
    return it. Treating flicker as a safety metric would reject a well-behaved model for being
    honest about its uncertainty.

─── Raw and normalised, always side by side ───────────────────────────────────────────────────

plan.md §4: "WER/CER normalised and raw (raw is what matters for `llama.cpp` vs `llama dot
cpp`; normalisation hides exactly the errors we care about)."

Every WER/CER call here returns both, in one object, and :meth:`ErrorRates.to_dict` emits both.
There is no function that returns only the normalised figure, deliberately — a caller that wants
one number will otherwise pick the flattering one.

The normaliser (:data:`NORMALISER_VERSION`) is an explicit, versioned transformation, not an
incidental cleanup. What it deliberately does **not** do is as load-bearing as what it does; see
:func:`normalise_text`. In particular it never rewrites spelled punctuation ("dot" → "."), so it
cannot rescue "llama dot cpp". A normaliser that rescued it would report 0 % WER for precisely
the failure mode this product exists to fix. ``test_metrics.py`` pins that.

─── Versioning ────────────────────────────────────────────────────────────────────────────────

Three rulesets can change a historical number without changing a single measurement: the
normaliser, the technical-term matching rules, and the grapheme approximation. All three carry a
version string, :func:`metric_versions` returns them as a dict, and ``run_matrix.py`` writes that
dict into every result cell. A later rule change then shows up as a version mismatch instead of
silently rewriting history.

─── Privacy ───────────────────────────────────────────────────────────────────────────────────

Several reports here carry verbatim transcript text — :class:`IcerReport.committed_text`, the
per-case output of :func:`hallucination_report` — because the text is the diagnostic. plan.md
§1.9 makes result files containing verbatim transcripts gitignored; the fields that carry text
are marked in ``to_dict`` output under the ``"contains_transcript_text"`` flag so the runner can
route them correctly.

─── The grapheme approximation ────────────────────────────────────────────────────────────────

ICER counts *graphemes*, and the count must match what the client counts, or the headline safety
number measures a different quantity than the thing it is meant to gate. The segmentation below
is the same declared approximation as ``src/util/utf8.h`` and ``server/kvit_asr/protocol.py``:
same forced-extend ranges, same Extended_Pictographic range approximation, same omissions
(GB9b Prepend, GB6–GB8 Hangul composition, GB9c Indic conjuncts).

It is a **third** copy of that rule set, and that is a real cost, accepted for a stated reason:
``bench/`` must run on a bare interpreter with only this file's directory on ``sys.path``, and
importing the sidecar would make the benchmark harness depend on the sidecar's package layout
while it is still moving. ``test_metrics.py`` cross-checks this implementation against
``kvit_asr.protocol.grapheme_boundaries`` when the sidecar is importable and *fails* on
divergence, which is the mechanism that keeps the copies honest. If that test ever cannot find
the sidecar it reports a skip with a reason rather than passing quietly.

The Unicode-version question docs/protocol.md §14 item 2 records is unresolved here too, and
:data:`GRAPHEME_RULESET_VERSION` says so rather than asserting a version we have not pinned.
"""

from __future__ import annotations

import math
import re
import statistics
import unicodedata
from collections import Counter
from dataclasses import dataclass, field
from typing import Iterable, Sequence

__all__ = [
    # versions
    "METRICS_VERSION", "NORMALISER_VERSION", "TERM_RULESET_VERSION",
    "GRAPHEME_RULESET_VERSION", "metric_versions",
    # text handling
    "grapheme_clusters", "grapheme_boundaries", "count_graphemes", "utf8_len",
    "normalise_text", "normalise_tokens",
    # error rates
    "EditOps", "ErrorRates", "edit_ops", "aggregate_edit_ops",
    "word_error_ops", "char_error_ops", "error_rates", "aggregate_error_rates",
    # technical terms
    "TechnicalTerm", "TERMS", "TERM_MATCH_RULES", "TermMatcher",
    "TermF1Report", "TermDetail", "term_f1",
    # ICER / flicker
    "CommitEvent", "IcerReport", "icer",
    "HypothesisEvent", "FlickerReport", "flicker",
    # latency and resources
    "LATENCY_STAGES", "LatencySample", "Spread", "LatencyReport", "summarise_latency",
    "real_time_factor", "ResourceReport",
    # hallucination
    "HallucinationCase", "HallucinationReport", "is_hallucination", "hallucination_report",
    # gating
    "select_gate_metrics",
]

METRICS_VERSION = "metrics-1"

# ══════════════════════════════════════════════════════════════════════════════════════════════
# 1. UTF-8 and grapheme clusters
# ══════════════════════════════════════════════════════════════════════════════════════════════

#: Not a Unicode version number. src/util/utf8.h's `kUnicodeVersion` currently reads
#: "unspecified-pending-protocol-md-14.2" and this string mirrors it deliberately: docs/protocol.md
#: §14 item 2 has not decided which Unicode property tables both endpoints pin, and writing a
#: number here would assert a decision nobody has made. The trailing component is this file's own
#: revision of the rule set, so a change here is visible in every result cell even when the
#: Unicode question is still open.
GRAPHEME_RULESET_VERSION = "utf8-approx-1/unspecified-pending-protocol-md-14.2"

_ZWJ = "\u200d"

# The normative floor from docs/protocol.md §8.4, as explicit ranges so it does not move when the
# interpreter's `unicodedata` tables move. Kept byte-for-byte in step with the same table in
# server/kvit_asr/protocol.py and src/util/utf8.cpp.
_FORCED_EXTEND_RANGES: tuple[tuple[int, int], ...] = (
    (0x0300, 0x036F),    # combining diacritical marks
    (0x200D, 0x200D),    # ZWJ — also handled by name in the break rules
    (0x20E3, 0x20E3),    # combining enclosing keycap
    (0xFE00, 0xFE0F),    # variation selectors 1–16
    (0x1F3FB, 0x1F3FF),  # emoji skin-tone modifiers (category Sk, but Grapheme_Extend=Yes)
    (0xE0100, 0xE01EF),  # variation selectors 17–256
)

# Extended_Pictographic approximated by ranges rather than a generated table. Applied slightly
# more broadly than UAX #29 specifies, which is conservative in the direction that matters: it can
# merge two adjacent emoji into one cluster (undercounting graphemes) but never split one.
_EXT_PICTOGRAPHIC_RANGES: tuple[tuple[int, int], ...] = (
    (0x00A9, 0x00A9), (0x00AE, 0x00AE), (0x203C, 0x203C), (0x2049, 0x2049),
    (0x2122, 0x2122), (0x2139, 0x2139), (0x2194, 0x21AA), (0x231A, 0x231B),
    (0x2328, 0x2328), (0x23CF, 0x23CF), (0x23E9, 0x23FA), (0x24C2, 0x24C2),
    (0x25AA, 0x25AB), (0x25B6, 0x25B6), (0x25C0, 0x25C0), (0x25FB, 0x25FE),
    (0x2600, 0x27BF), (0x2934, 0x2935), (0x2B00, 0x2BFF), (0x3030, 0x3030),
    (0x303D, 0x303D), (0x3297, 0x3297), (0x3299, 0x3299),
    (0x1F000, 0x1FAFF), (0x1FC00, 0x1FFFD),
)

_REGIONAL_INDICATOR = (0x1F1E6, 0x1F1FF)


def _in_ranges(cp: int, ranges: tuple[tuple[int, int], ...]) -> bool:
    for lo, hi in ranges:
        if lo <= cp <= hi:
            return True
        if cp < lo:
            break  # ranges are sorted
    return False


def _is_extend(ch: str) -> bool:
    """Grapheme_Extend=Yes, approximated: the explicit floor, then categories Mn/Me."""
    if _in_ranges(ord(ch), _FORCED_EXTEND_RANGES):
        return True
    return unicodedata.category(ch) in ("Mn", "Me")


def _is_spacing_mark(ch: str) -> bool:
    """Grapheme_Cluster_Break=SpacingMark, approximated by category Mc (over-eager at joining)."""
    return unicodedata.category(ch) == "Mc"


def _is_ext_pictographic(ch: str) -> bool:
    return _in_ranges(ord(ch), _EXT_PICTOGRAPHIC_RANGES)


def _is_regional_indicator(ch: str) -> bool:
    return _REGIONAL_INDICATOR[0] <= ord(ch) <= _REGIONAL_INDICATOR[1]


def _utf8_char_len(ch: str) -> int:
    cp = ord(ch)
    if cp < 0x80:
        return 1
    if cp < 0x800:
        return 2
    if cp < 0x10000:
        return 3
    return 4


def utf8_len(text: str) -> int:
    """Length of `text` in UTF-8 bytes — the unit every protocol offset is measured in."""
    return sum(_utf8_char_len(ch) for ch in text)


def _breaks_between(prev: str, cur: str, ri_run: int, zwj_after_pict: bool) -> bool:
    if prev == "\r" and cur == "\n":
        return False                                            # GB3
    if cur == _ZWJ or _is_extend(cur):
        return False                                            # GB9
    if _is_spacing_mark(cur):
        return False                                            # GB9a
    if zwj_after_pict and _is_ext_pictographic(cur):
        return False                                            # GB11
    if _is_regional_indicator(prev) and _is_regional_indicator(cur) and ri_run % 2 == 1:
        return False                                            # GB12/GB13
    return True


def grapheme_clusters(text: str) -> list[str]:
    """Split `text` into extended grapheme clusters under the declared approximation.

    Rules honoured: GB3 (CR LF), GB9 (Extend and ZWJ), GB9a (SpacingMark), GB11 (ZWJ emoji
    sequences), GB12/GB13 (regional-indicator pairs). Rules deliberately not honoured, matching
    src/util/utf8.h so the two do not diverge: GB4/GB5 (Control/CR/LF breaks), GB9b (Prepend),
    GB6–GB8 (Hangul jamo composition), GB9c (Indic conjuncts).

    Every omission fails in the same direction — more clusters than UAX #29 would report, never
    fewer within a sequence that must not be split. For ICER that direction inflates the
    denominator slightly rather than hiding a corruption, which is the side to be wrong on.
    """
    if not text:
        return []

    out: list[str] = []
    cur: list[str] = []
    prev: str | None = None
    ri_run = 0             # consecutive regional indicators ending at `prev`
    pict_extend = False    # the run ending at `prev` matches  ExtPict Extend*
    zwj_after_pict = False  # the run ending at `prev` matches  ExtPict Extend* ZWJ

    for ch in text:
        if prev is not None and _breaks_between(prev, ch, ri_run, zwj_after_pict):
            out.append("".join(cur))
            cur = []

        # Advance the running classification. ZWJ is also Grapheme_Extend, so it must be
        # recognised by name before the generic Extend branch.
        if ch == _ZWJ:
            zwj_after_pict = pict_extend
            pict_extend = False
        elif _is_ext_pictographic(ch):
            pict_extend = True
            zwj_after_pict = False
        elif _is_extend(ch):
            zwj_after_pict = False   # pict_extend is carried: ExtPict Extend* stays a pict run
        else:
            pict_extend = False
            zwj_after_pict = False

        ri_run = ri_run + 1 if _is_regional_indicator(ch) else 0
        cur.append(ch)
        prev = ch

    out.append("".join(cur))
    return out


def grapheme_boundaries(text: str) -> tuple[int, ...]:
    """Every cluster boundary as a UTF-8 byte offset, always including 0 and ``utf8_len(text)``.

    Byte offsets, not string indices, because that is what crosses the wire. Present mainly so
    ``test_metrics.py`` can compare this module against ``kvit_asr.protocol`` field for field.
    """
    out = [0]
    off = 0
    for cluster in grapheme_clusters(text):
        off += utf8_len(cluster)
        out.append(off)
    return tuple(out)


def count_graphemes(text: str) -> int:
    return len(grapheme_clusters(text))


def _common_prefix_len(a: Sequence[object], b: Sequence[object]) -> int:
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 2. The normaliser — explicit, versioned, and deliberately limited
# ══════════════════════════════════════════════════════════════════════════════════════════════

#: Bump when any rule in :func:`normalise_text` changes. Written into every result cell so a rule
#: change cannot silently make old and new numbers look comparable.
NORMALISER_VERSION = "norm-1"

#: Punctuation stripped from the *edges* of a token. Internal punctuation is never touched — that
#: is the single most important line in this file. `-` `_` `/` `\` `+` `#` `*` `~` `@` `$` `%` `&`
#: `=` `|` `^` are absent on purpose: they carry meaning inside identifiers, paths and CLI flags
#: (`--n-gpu-layers`, `sm_120`, `C:\Users`, `Win+H`, `#29`).
EDGE_PUNCT = frozenset(".,;:!?\"'()[]{}\u2026\u201c\u201d\u2018\u2019\u00ab\u00bb\u2014\u2013")

# Cardinal number words the normaliser folds to digits. Deliberately stops at 99: dictation of a
# version number, a layer count or a model size lands in this range, and composing "one hundred
# and twenty three" correctly needs a real number parser whose failure modes would be a new source
# of divergence between raw and normalised. Anything outside the table stays as words in both.
_UNITS = {
    "zero": 0, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7,
    "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13,
    "fourteen": 14, "fifteen": 15, "sixteen": 16, "seventeen": 17, "eighteen": 18,
    "nineteen": 19,
}
_TENS = {
    "twenty": 20, "thirty": 30, "forty": 40, "fifty": 50,
    "sixty": 60, "seventy": 70, "eighty": 80, "ninety": 90,
}
_SMALL_UNITS = {k: v for k, v in _UNITS.items() if 1 <= v <= 9}

_GROUPED_DIGITS = re.compile(r"^\d{1,3}(?:,\d{3})+$")
_TENS_HYPHEN_UNIT = re.compile(r"^([a-z]+)-([a-z]+)$")


def _strip_edges(token: str) -> str:
    """Remove leading and trailing :data:`EDGE_PUNCT` runs. Internal punctuation survives."""
    start, end = 0, len(token)
    while start < end and token[start] in EDGE_PUNCT:
        start += 1
    while end > start and token[end - 1] in EDGE_PUNCT:
        end -= 1
    return token[start:end]


def normalise_tokens(text: str) -> list[str]:
    """The normalised token stream. :func:`normalise_text` is this joined with single spaces.

    **What it does**, in order, each step chosen because it removes a difference that dictation
    cannot be expected to convey:

    1. Unicode NFC. Not NFKC: NFKC rewrites compatibility characters (fullwidth forms,
       superscripts, ligatures), which is a content change in text that is full of identifiers.
    2. Split on whitespace, and collapse runs of it.
    3. Strip :data:`EDGE_PUNCT` from each token's edges — a sentence-final full stop, a comma, a
       closing bracket. A speaker does not dictate these reliably and a model does not emit them
       reliably.
    4. ``str.casefold()``. Case is not recoverable from speech.
    5. Digit-group separators removed (``1,000`` → ``1000``) and cardinal number words 0–99 folded
       to digits, including the ``thirty-five`` and ``thirty five`` forms.

    **What it deliberately does not do**, which is the part that makes it fit for purpose:

    * **No spelled-punctuation expansion.** "dot" never becomes ".", "dash" never becomes "-",
      "slash", "underscore", "colon", "hash" never become their symbols. This is the rule that
      keeps "llama dot cpp" scoring as an error against "llama.cpp" *after* normalisation. plan.md
      §4 names that exact pair as the error class this product exists to fix; a normaliser that
      folded it away would report the product's central failure as a perfect score.
    * **No internal punctuation removal.** "llama.cpp" is not "llama cpp" and not "llamacpp".
    * No stemming, no stopword removal, no acronym expansion, no spelling-variant mapping, no
      contraction expansion. Each of those hides an error class somebody cares about, and none of
      them is needed to compare two ASR systems on the same reference.

    The result: normalised WER is a genuinely easier task than raw WER on *formatting*, and
    exactly as hard on *content*. That is the intended gap between the two numbers.
    """
    text = unicodedata.normalize("NFC", text)
    raw_tokens = [t for t in (_strip_edges(tok) for tok in text.split()) if t]
    folded = [t.casefold() for t in raw_tokens]

    out: list[str] = []
    i = 0
    while i < len(folded):
        tok = folded[i]

        # "1,000" -> "1000". Only for a fully grouped literal, so "3,x" is left alone.
        if _GROUPED_DIGITS.match(tok):
            out.append(tok.replace(",", ""))
            i += 1
            continue

        # "thirty-five" -> "35". Handled explicitly because step 3 preserves internal hyphens,
        # which is right for "--n-gpu-layers" and wrong for a spoken compound number.
        m = _TENS_HYPHEN_UNIT.match(tok)
        if m and m.group(1) in _TENS and m.group(2) in _SMALL_UNITS:
            out.append(str(_TENS[m.group(1)] + _SMALL_UNITS[m.group(2)]))
            i += 1
            continue

        # "thirty five" -> "35", consuming two tokens.
        if tok in _TENS and i + 1 < len(folded) and folded[i + 1] in _SMALL_UNITS:
            out.append(str(_TENS[tok] + _SMALL_UNITS[folded[i + 1]]))
            i += 2
            continue

        if tok in _TENS:
            out.append(str(_TENS[tok]))
            i += 1
            continue
        if tok in _UNITS:
            out.append(str(_UNITS[tok]))
            i += 1
            continue

        out.append(tok)
        i += 1
    return out


def normalise_text(text: str) -> str:
    """:func:`normalise_tokens` joined with single spaces. See there for the full rule list."""
    return " ".join(normalise_tokens(text))


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 3. Edit distance, WER, CER
# ══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class EditOps:
    """A Levenshtein alignment, decomposed. Uniform unit cost for all three operations.

    Orientation, stated because it is the usual source of a sign error: `reference` is the truth,
    `hypothesis` is what the system produced. A *deletion* is a reference element the hypothesis
    failed to produce; an *insertion* is a hypothesis element with nothing behind it.
    """

    hits: int
    substitutions: int
    deletions: int
    insertions: int
    ref_len: int
    hyp_len: int

    @property
    def errors(self) -> int:
        return self.substitutions + self.deletions + self.insertions

    @property
    def rate(self) -> float | None:
        """errors / ref_len, or ``None`` when the reference is empty.

        ``None`` rather than 0.0 or infinity. An empty reference with a non-empty hypothesis has
        an undefined *rate* but a perfectly well-defined *error count*, and pooling counts across
        a corpus (:func:`aggregate_edit_ops`) is the correct way to get a corpus figure anyway.
        Returning a number here would let one empty-reference clip silently define the aggregate.
        """
        if self.ref_len == 0:
            return 0.0 if self.hyp_len == 0 else None
        return self.errors / self.ref_len

    def to_dict(self) -> dict:
        return {
            "hits": self.hits,
            "substitutions": self.substitutions,
            "deletions": self.deletions,
            "insertions": self.insertions,
            "errors": self.errors,
            "ref_len": self.ref_len,
            "hyp_len": self.hyp_len,
            "rate": self.rate,
        }


def _build_matrix(ref: Sequence[object], hyp: Sequence[object]) -> list[list[int]]:
    n, m = len(ref), len(hyp)
    d = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        d[i][0] = i
    for j in range(1, m + 1):
        d[0][j] = j
    for i in range(1, n + 1):
        ri = ref[i - 1]
        di, dprev = d[i], d[i - 1]
        for j in range(1, m + 1):
            sub = dprev[j - 1] + (0 if ri == hyp[j - 1] else 1)
            di[j] = min(sub, dprev[j] + 1, di[j - 1] + 1)
    return d


def _backtrack(d: list[list[int]], ref: Sequence[object], hyp: Sequence[object],
               i: int, j: int) -> tuple[int, int, int, int]:
    """Count (hits, sub, del, ins) on the optimal path ending at ``d[i][j]``.

    Tie-break order is match/substitution, then deletion, then insertion — fixed so that two runs
    over the same data report the same decomposition. The *total* is tie-break independent; the
    split between the three op types is not, which is why it is pinned rather than left to
    whatever ``min`` happens to pick.
    """
    hits = sub = dele = ins = 0
    while i > 0 or j > 0:
        if i > 0 and j > 0:
            cost = 0 if ref[i - 1] == hyp[j - 1] else 1
            if d[i][j] == d[i - 1][j - 1] + cost:
                if cost:
                    sub += 1
                else:
                    hits += 1
                i -= 1
                j -= 1
                continue
        if i > 0 and d[i][j] == d[i - 1][j] + 1:
            dele += 1
            i -= 1
            continue
        ins += 1
        j -= 1
    return hits, sub, dele, ins


def edit_ops(ref: Sequence[object], hyp: Sequence[object], *,
             free_reference_tail: bool = False) -> EditOps:
    """Levenshtein alignment of `hyp` against `ref`, decomposed into operations.

    `free_reference_tail` lets the *end* of the reference go unconsumed at no cost. It exists for
    ICER: a commit trace is legitimately a prefix of the final text, and the uncommitted tail is
    not an error — it is text the session had not got to yet. It is off by default because for
    WER/CER an unproduced tail is a real deletion.
    """
    d = _build_matrix(ref, hyp)
    n, m = len(ref), len(hyp)

    if free_reference_tail:
        # Consume all of `hyp`, any prefix of `ref`. Smallest i on a tie: at equal cost that
        # treats more of the reference as "not reached yet", which is the honest reading of an
        # uncommitted tail. The cost — and therefore the reported error count — is identical
        # either way; only determinism is at stake.
        best_i, best = 0, d[0][m]
        for i in range(1, n + 1):
            if d[i][m] < best:
                best_i, best = i, d[i][m]
        n = best_i

    hits, sub, dele, ins = _backtrack(d, ref, hyp, n, m)
    return EditOps(hits=hits, substitutions=sub, deletions=dele, insertions=ins,
                   ref_len=n, hyp_len=m)


def aggregate_edit_ops(items: Iterable[EditOps]) -> EditOps:
    """Pool counts across clips. The corpus rate is pooled errors over pooled reference length —
    never the mean of per-clip rates, which weights a three-word clip like a thirty-word one."""
    hits = sub = dele = ins = ref = hyp = 0
    for it in items:
        hits += it.hits
        sub += it.substitutions
        dele += it.deletions
        ins += it.insertions
        ref += it.ref_len
        hyp += it.hyp_len
    return EditOps(hits, sub, dele, ins, ref, hyp)


def word_error_ops(reference: str, hypothesis: str, *, normalised: bool) -> EditOps:
    """Word-level alignment. Raw splits on whitespace and changes nothing else."""
    if normalised:
        return edit_ops(normalise_tokens(reference), normalise_tokens(hypothesis))
    return edit_ops(reference.split(), hypothesis.split())


def char_error_ops(reference: str, hypothesis: str, *, normalised: bool) -> EditOps:
    """Character-level alignment, where "character" means **grapheme cluster**.

    Not code points: a CER that counts "é" as one unit when composed and two when decomposed is
    measuring an encoding choice, not a recognition error. The unit is the same one ICER uses.
    """
    ref = normalise_text(reference) if normalised else reference
    hyp = normalise_text(hypothesis) if normalised else hypothesis
    return edit_ops(grapheme_clusters(ref), grapheme_clusters(hyp))


@dataclass(frozen=True)
class ErrorRates:
    """WER and CER, raw and normalised, in one object because they must be read together."""

    wer_raw: EditOps
    wer_normalised: EditOps
    cer_raw: EditOps
    cer_normalised: EditOps

    def to_dict(self) -> dict:
        return {
            "wer_raw": self.wer_raw.to_dict(),
            "wer_normalised": self.wer_normalised.to_dict(),
            "cer_raw": self.cer_raw.to_dict(),
            "cer_normalised": self.cer_normalised.to_dict(),
            "normaliser_version": NORMALISER_VERSION,
            "grapheme_ruleset_version": GRAPHEME_RULESET_VERSION,
            "note": (
                "raw is the figure plan.md §4 gates the accuracy discussion on; normalised is "
                "reported alongside it so formatting differences can be separated from content "
                "errors, never so the smaller number can be quoted alone"
            ),
        }


def error_rates(reference: str, hypothesis: str) -> ErrorRates:
    return ErrorRates(
        wer_raw=word_error_ops(reference, hypothesis, normalised=False),
        wer_normalised=word_error_ops(reference, hypothesis, normalised=True),
        cer_raw=char_error_ops(reference, hypothesis, normalised=False),
        cer_normalised=char_error_ops(reference, hypothesis, normalised=True),
    )


def aggregate_error_rates(items: Iterable[ErrorRates]) -> ErrorRates:
    items = list(items)
    return ErrorRates(
        wer_raw=aggregate_edit_ops(i.wer_raw for i in items),
        wer_normalised=aggregate_edit_ops(i.wer_normalised for i in items),
        cer_raw=aggregate_edit_ops(i.cer_raw for i in items),
        cer_normalised=aggregate_edit_ops(i.cer_normalised for i in items),
    )


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 4. Technical-term F1 — rules fixed in advance, as data
# ══════════════════════════════════════════════════════════════════════════════════════════════

#: Bump when :data:`TERMS` or :data:`TERM_MATCH_RULES` changes. Written into every result cell.
#: plan.md §4 requires the matching rules be fixed *before* the corpus is run, and this string is
#: the mechanism: a rule change after the fact produces cells with two different versions rather
#: than one incomparable series.
TERM_RULESET_VERSION = "terms-1"

#: The rules, in the output, in English. Auditability is the point: a reader of a result cell can
#: see what "matched" meant without reading this file.
TERM_MATCH_RULES: tuple[str, ...] = (
    "R1 case: folded by default, because speech does not convey case; a term whose case is "
    "semantically load-bearing sets case_sensitive=True and is then compared exactly.",
    "R2 internal punctuation is significant and is never normalised away: 'llama.cpp' is not "
    "'llama cpp' and not 'llamacpp'.",
    "R3 edge punctuation (EDGE_PUNCT) is stripped from both sides of every token before "
    "matching, unconditionally and independently of the term table, so the rule cannot be "
    "tuned per term after seeing results.",
    "R4 aliases are written-form equivalents only (e.g. 'UTF8' for 'UTF-8'). A phonetic "
    "rendering of punctuation is never an alias; those are listed as wrong_forms and count as "
    "a miss with a named reason.",
    "R5 compound identifiers and multi-word terms match as token n-grams, longest form first, "
    "left to right, each token consumed at most once.",
    "R6 counting is multiset per term: tp = min(ref, hyp), fp = max(0, hyp - ref), "
    "fn = max(0, ref - hyp), pooled across terms before precision/recall.",
)


@dataclass(frozen=True)
class TechnicalTerm:
    """One entry in the pre-registered term table.

    `wrong_forms` is not decoration. A term that is missed because the model wrote "llama dot
    cpp" is a different diagnosis from a term that is missed because the model wrote nothing, and
    plan.md §4 exists to distinguish exactly those. Recording the known-bad renderings turns a
    miss into a named failure instead of an absence.
    """

    canonical: str
    aliases: tuple[str, ...] = ()
    wrong_forms: tuple[str, ...] = ()
    case_sensitive: bool = False
    note: str = ""

    @property
    def forms(self) -> tuple[str, ...]:
        return (self.canonical,) + self.aliases


#: The term table. These are the strings plan.md §4 says Win+H gets wrong and public benchmarks
#: do not measure: identifiers, paths, CLI flags, model numbers, acronyms. It is data so that
#: adding a term is a data change with a version bump, not a code change.
TERMS: tuple[TechnicalTerm, ...] = (
    TechnicalTerm("llama.cpp", aliases=(),
                  wrong_forms=("llama dot cpp", "llama cpp", "llamacpp", "lama.cpp"),
                  note="the motivating example in plan.md §4"),
    TechnicalTerm("whisper.cpp", wrong_forms=("whisper dot cpp", "whisper cpp", "whispercpp")),
    TechnicalTerm("sherpa-onnx", aliases=("sherpa onnx",),
                  wrong_forms=("sherpa dash onnx", "sherpaonnx")),
    TechnicalTerm("onnxruntime", aliases=("onnx runtime",)),
    TechnicalTerm("ONNX", aliases=("onnx",)),
    TechnicalTerm("sm_120", aliases=("sm120",),
                  wrong_forms=("sm underscore 120", "sm 120", "s m 120")),
    TechnicalTerm("--n-gpu-layers", aliases=("-n-gpu-layers",),
                  wrong_forms=("dash dash n gpu layers", "n gpu layers", "--ngpulayers")),
    TechnicalTerm("C:\\Users\\sk\\AppData", aliases=("c:\\users\\sk\\appdata",),
                  wrong_forms=("c colon backslash users backslash sk backslash appdata",
                               "c users sk appdata")),
    TechnicalTerm("RTX", aliases=("rtx",)),
    TechnicalTerm("5090"),
    TechnicalTerm("UTF-8", aliases=("UTF8", "utf-8", "utf8"),
                  wrong_forms=("utf dash 8", "u t f 8")),
    TechnicalTerm("Qwen3-ASR", aliases=("qwen3 asr", "qwen 3 asr"),
                  wrong_forms=("gwen 3 asr", "quinn 3 asr")),
    TechnicalTerm("Nemotron", aliases=("nemotron",), wrong_forms=("nemo tron", "nimotron")),
    TechnicalTerm("parakeet-tdt-0.6b-v2", aliases=("parakeet tdt 0.6b v2",)),
    TechnicalTerm("distil-large-v3", aliases=("distil large v3",),
                  wrong_forms=("distill-large-v3", "distil large version 3")),
    TechnicalTerm("faster-whisper", aliases=("faster whisper",)),
    TechnicalTerm("CTranslate2", aliases=("ctranslate2", "ctranslate 2")),
    TechnicalTerm("WASAPI", aliases=("wasapi",), wrong_forms=("wa sappy", "wassapi")),
    TechnicalTerm("SendInput", aliases=("sendinput", "send input")),
    TechnicalTerm("BitBlt", aliases=("bitblt", "bit blt")),
    TechnicalTerm("Direct2D", aliases=("direct2d", "direct 2d")),
    TechnicalTerm("DirectWrite", aliases=("directwrite", "direct write")),
    TechnicalTerm("WSLg", aliases=("wslg",), wrong_forms=("wsl g", "wsl gee")),
    TechnicalTerm("Win+H", aliases=("win+h",), wrong_forms=("win plus h", "windows h")),
    TechnicalTerm("Win+Y", aliases=("win+y",), wrong_forms=("win plus y", "windows y")),
    TechnicalTerm("HF_HOME", aliases=("hf_home",), wrong_forms=("hf home", "h f home")),
    TechnicalTerm("UV_CACHE_DIR", aliases=("uv_cache_dir",), wrong_forms=("uv cache dir",)),
    TechnicalTerm("CMake", aliases=("cmake",), wrong_forms=("see make", "c make")),
    TechnicalTerm("MSVC", aliases=("msvc",)),
    TechnicalTerm("s16le", wrong_forms=("s 16 le", "s16 le")),
    TechnicalTerm("16 kHz", aliases=("16khz", "16 khz"), wrong_forms=("sixteen kilohertz",),
                  note="the number-word form is a wrong_form, not an alias: the normaliser folds "
                       "'sixteen' to '16' for WER, but a term table that accepted the spelled "
                       "form would stop measuring whether the model writes units correctly"),
    # No "vad." wrong form: R3 strips the trailing full stop before matching, so it would be a
    # dead table entry. Wrong forms must be things R3 cannot already fix.
    TechnicalTerm("VAD", aliases=("vad",), wrong_forms=("v a d",)),
    TechnicalTerm("kebab-case", aliases=("kebab case",)),
    TechnicalTerm("UAX #29", aliases=("uax #29", "uax 29")),
)


@dataclass(frozen=True)
class TermDetail:
    canonical: str
    ref_count: int
    hyp_count: int
    tp: int
    fp: int
    fn: int
    wrong_forms_seen: tuple[str, ...] = ()

    def to_dict(self) -> dict:
        return {
            "term": self.canonical, "ref": self.ref_count, "hyp": self.hyp_count,
            "tp": self.tp, "fp": self.fp, "fn": self.fn,
            "wrong_forms_seen": list(self.wrong_forms_seen),
        }


@dataclass(frozen=True)
class TermF1Report:
    tp: int
    fp: int
    fn: int
    per_term: tuple[TermDetail, ...]
    ruleset_version: str = TERM_RULESET_VERSION

    @property
    def precision(self) -> float:
        return self.tp / (self.tp + self.fp) if (self.tp + self.fp) else 0.0

    @property
    def recall(self) -> float:
        return self.tp / (self.tp + self.fn) if (self.tp + self.fn) else 0.0

    @property
    def f1(self) -> float:
        p, r = self.precision, self.recall
        return 2 * p * r / (p + r) if (p + r) else 0.0

    def to_dict(self) -> dict:
        return {
            "tp": self.tp, "fp": self.fp, "fn": self.fn,
            "precision": self.precision, "recall": self.recall, "f1": self.f1,
            "ruleset_version": self.ruleset_version,
            "rules": list(TERM_MATCH_RULES),
            "per_term": [d.to_dict() for d in self.per_term if d.ref_count or d.hyp_count],
        }


def _tokenise_for_terms(text: str) -> list[str]:
    """Whitespace split, NFC, edge punctuation stripped (R3). Internal punctuation preserved."""
    text = unicodedata.normalize("NFC", text)
    return [t for t in (_strip_edges(tok) for tok in text.split()) if t]


class TermMatcher:
    """Finds occurrences of :data:`TERMS` (and their known wrong forms) in a text.

    Longest-n-gram-first, left to right, each token consumed at most once (R5). A greedy longest
    match is what stops "UTF-8" inside "UTF-8 BOM" being counted twice and what makes the count
    independent of the order terms happen to appear in the table.
    """

    def __init__(self, terms: Sequence[TechnicalTerm] = TERMS) -> None:
        self.terms = tuple(terms)
        self._exact: dict[tuple[str, ...], str] = {}   # case-sensitive terms
        self._folded: dict[tuple[str, ...], str] = {}  # case-folded terms
        self._wrong_folded: dict[tuple[str, ...], tuple[str, str]] = {}  # -> (canonical, form)
        self._max_n = 1
        for term in self.terms:
            for form in term.forms:
                key = tuple(_tokenise_for_terms(form))
                if not key:
                    continue
                self._max_n = max(self._max_n, len(key))
                if term.case_sensitive:
                    self._exact.setdefault(key, term.canonical)
                else:
                    self._folded.setdefault(tuple(k.casefold() for k in key), term.canonical)
            for form in term.wrong_forms:
                key = tuple(k.casefold() for k in _tokenise_for_terms(form))
                if not key:
                    continue
                self._max_n = max(self._max_n, len(key))
                self._wrong_folded.setdefault(key, (term.canonical, form))

    def find(self, text: str) -> list[str]:
        """Canonical names of the term occurrences in `text`, in order."""
        return [c for c, _ in self._scan(text)[0]]

    def find_wrong_forms(self, text: str) -> list[tuple[str, str]]:
        """(canonical, wrong form) pairs found in `text`, in order.

        Scanned on the tokens *not* consumed by a correct match, so "llama.cpp" in a hypothesis
        that also says "llama dot cpp" elsewhere reports one of each rather than shadowing one.
        """
        return self._scan(text)[1]

    def _scan(self, text: str) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
        tokens = _tokenise_for_terms(text)
        folded = [t.casefold() for t in tokens]
        hits: list[tuple[str, str]] = []
        wrong: list[tuple[str, str]] = []
        i = 0
        n = len(tokens)
        while i < n:
            matched = 0
            for size in range(min(self._max_n, n - i), 0, -1):
                exact_key = tuple(tokens[i:i + size])
                if exact_key in self._exact:
                    hits.append((self._exact[exact_key], " ".join(exact_key)))
                    matched = size
                    break
                fold_key = tuple(folded[i:i + size])
                if fold_key in self._folded:
                    hits.append((self._folded[fold_key], " ".join(exact_key)))
                    matched = size
                    break
                if fold_key in self._wrong_folded:
                    canonical, form = self._wrong_folded[fold_key]
                    wrong.append((canonical, form))
                    matched = size
                    break
            i += matched if matched else 1
        return hits, wrong


def term_f1(reference: str, hypothesis: str, *,
            matcher: TermMatcher | None = None) -> TermF1Report:
    """Technical-term F1 under the pre-registered rules.

    The reference's term occurrences are found with the same matcher as the hypothesis's, so a
    term the table does not know about is invisible on both sides and cannot skew the score. That
    is a deliberate limitation: the metric measures the terms we said in advance we cared about,
    not "all technical-looking words", which would be a moving target.
    """
    m = matcher or TermMatcher()
    ref_counts = Counter(m.find(reference))
    hyp_counts = Counter(m.find(hypothesis))
    wrong_seen: dict[str, list[str]] = {}
    for canonical, form in m.find_wrong_forms(hypothesis):
        wrong_seen.setdefault(canonical, []).append(form)

    details: list[TermDetail] = []
    tp = fp = fn = 0
    for canonical in sorted(set(ref_counts) | set(hyp_counts) | set(wrong_seen)):
        r, h = ref_counts.get(canonical, 0), hyp_counts.get(canonical, 0)
        t, f_p, f_n = min(r, h), max(0, h - r), max(0, r - h)
        tp += t
        fp += f_p
        fn += f_n
        details.append(TermDetail(canonical, r, h, t, f_p, f_n,
                                  tuple(wrong_seen.get(canonical, ()))))
    return TermF1Report(tp=tp, fp=fp, fn=fn, per_term=tuple(details))


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 5. ICER — the headline safety number
# ══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class CommitEvent:
    """One injection into the target document: exactly the text that was typed, and when.

    `text` is what the injector actually sent, not what the commit engine intended to send. The
    whole point of ICER is to measure the document, so the trace has to come from the injection
    site.
    """

    text: str
    t_ms: float
    utterance_id: int | None = None
    seq: int | None = None


@dataclass(frozen=True)
class IcerReport:
    committed_text: str
    final_text: str
    committed_graphemes: int
    final_graphemes: int
    events: int
    prefix_invariant_held: bool
    divergence_grapheme_index: int | None
    divergence_byte_offset: int | None
    substitutions: int
    deletions: int
    insertions: int
    committed_after_divergence: int

    @property
    def errors(self) -> int:
        return self.substitutions + self.deletions + self.insertions

    @property
    def icer_per_1000(self) -> float:
        """The gating figure. Zero commits gives 0.0 — see :func:`icer` for why that is honest."""
        if self.committed_graphemes == 0:
            return 0.0
        return self.errors * 1000.0 / self.committed_graphemes

    @property
    def icer_strict_per_1000(self) -> float:
        """The pessimistic reading: everything committed at or after the first divergence.

        plan.md's wording — "committed graphemes that differ from the final hypothesis" — admits
        two readings, and rather than pick one silently this module reports both. The headline
        aligns the two texts and counts the differing graphemes. This one counts every committed
        grapheme from the first divergence onward, on the argument that once the document has
        gone wrong the tail is standing on a bad foundation. They coincide at zero, which is the
        value that matters most, and diverge on how bad a bad run is.
        """
        if self.committed_graphemes == 0:
            return 0.0
        return self.committed_after_divergence * 1000.0 / self.committed_graphemes

    def to_dict(self, *, include_text: bool = True) -> dict:
        out: dict = {
            "icer_per_1000": self.icer_per_1000,
            "icer_strict_per_1000": self.icer_strict_per_1000,
            "committed_graphemes": self.committed_graphemes,
            "final_graphemes": self.final_graphemes,
            "events": self.events,
            "errors": self.errors,
            "substitutions": self.substitutions,
            "deletions": self.deletions,
            "insertions": self.insertions,
            "committed_after_divergence": self.committed_after_divergence,
            "prefix_invariant_held": self.prefix_invariant_held,
            "divergence_grapheme_index": self.divergence_grapheme_index,
            "divergence_byte_offset": self.divergence_byte_offset,
            "no_commits": self.committed_graphemes == 0,
            "grapheme_ruleset_version": GRAPHEME_RULESET_VERSION,
            "gates": True,
        }
        if include_text:
            out["committed_text"] = self.committed_text
            out["final_text"] = self.final_text
            out["contains_transcript_text"] = True
        return out


def icer(commits: Sequence[CommitEvent], final_text: str) -> IcerReport:
    """Irrevocable committed error rate, per 1000 committed graphemes.

    Inputs are the commit trace (what was injected, in order, with timestamps) and the final text
    for the same utterance or session. Timestamps are carried through rather than used: ICER is
    an amount-of-damage metric, not a timing one, and a trace without ordering information cannot
    be checked against the prefix invariant at all.

    Method:

    1. Concatenate the trace in order into the committed text C. That concatenation *is* the
       document content this session produced — no reordering, no deduplication.
    2. Compare C's grapheme sequence with the final text F's. If C is a grapheme prefix of F the
       invariant held and there are zero errors. This is the case the whole design aims at and it
       is checked exactly, not inferred from an alignment cost.
    3. Otherwise align C against F allowing F's *tail* to go unconsumed at no cost — the
       uncommitted remainder is not damage — and count substitutions, deletions and insertions.
    4. Rate = errors × 1000 / committed graphemes.

    **Zero commits.** ``committed_graphemes == 0`` returns 0.0 with ``no_commits`` set, and does
    not divide by zero. That is honest — nothing was committed, so nothing irrevocable happened —
    but it is also why ICER can never be read alone: a configuration that commits nothing scores
    a perfect ICER and is useless. The selection rule in plan.md §4 pairs it with latency and
    term F1 for exactly this reason.
    """
    committed_text = "".join(c.text for c in commits)
    c = grapheme_clusters(committed_text)
    f = grapheme_clusters(final_text)

    lcp = _common_prefix_len(c, f)
    held = lcp == len(c)  # C is a grapheme prefix of F (equal counts as a prefix)

    if held:
        return IcerReport(
            committed_text=committed_text, final_text=final_text,
            committed_graphemes=len(c), final_graphemes=len(f), events=len(commits),
            prefix_invariant_held=True,
            divergence_grapheme_index=None, divergence_byte_offset=None,
            substitutions=0, deletions=0, insertions=0, committed_after_divergence=0,
        )

    ops = edit_ops(f, c, free_reference_tail=True)
    return IcerReport(
        committed_text=committed_text, final_text=final_text,
        committed_graphemes=len(c), final_graphemes=len(f), events=len(commits),
        prefix_invariant_held=False,
        divergence_grapheme_index=lcp,
        divergence_byte_offset=utf8_len("".join(c[:lcp])),
        substitutions=ops.substitutions, deletions=ops.deletions, insertions=ops.insertions,
        committed_after_divergence=len(c) - lcp,
    )


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 6. Flicker — recorded, explicitly not gating
# ══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class HypothesisEvent:
    """One partial hypothesis as it arrived: cumulative text for the utterance, and when."""

    text: str
    t_ms: float
    utterance_id: int | None = None
    seq: int | None = None


@dataclass(frozen=True)
class FlickerReport:
    hypotheses: int
    revisions: int
    retracted_graphemes: int
    max_retraction_graphemes: int
    final_graphemes: int

    @property
    def flicker_per_1000(self) -> float:
        if self.final_graphemes == 0:
            return 0.0
        return self.retracted_graphemes * 1000.0 / self.final_graphemes

    def to_dict(self) -> dict:
        return {
            "flicker_per_1000": self.flicker_per_1000,
            "hypotheses": self.hypotheses,
            "revisions": self.revisions,
            "retracted_graphemes": self.retracted_graphemes,
            "max_retraction_graphemes": self.max_retraction_graphemes,
            "final_graphemes": self.final_graphemes,
            "gates": False,
            "note": (
                "diagnostic only. Flicker is churn inside our own overlay, which is what the "
                "overlay exists to absorb; it can be high with zero document corruption. ICER "
                "gates (plan.md §1.4). Do not use this number to reject a configuration."
            ),
        }


def flicker(hypotheses: Sequence[HypothesisEvent]) -> FlickerReport:
    """Grapheme churn across a partial-hypothesis stream. **Does not gate** — see the module docstring.

    A *revision* is a consecutive pair where the later hypothesis is not an extension of the
    earlier one. `retracted_graphemes` sums, over all pairs, how many graphemes of the earlier
    hypothesis the later one withdrew. The rate is expressed per 1000 graphemes of the final
    text, so a long utterance is not penalised for being long.
    """
    revisions = 0
    retracted = 0
    worst = 0
    prev: list[str] | None = None
    for h in hypotheses:
        cur = grapheme_clusters(h.text)
        if prev is not None:
            lcp = _common_prefix_len(prev, cur)
            back = len(prev) - lcp
            if back > 0:
                revisions += 1
                retracted += back
                worst = max(worst, back)
        prev = cur
    return FlickerReport(
        hypotheses=len(hypotheses),
        revisions=revisions,
        retracted_graphemes=retracted,
        max_retraction_graphemes=worst,
        final_graphemes=len(prev) if prev is not None else 0,
    )


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 7. Latency decomposition, RTF, resources
# ══════════════════════════════════════════════════════════════════════════════════════════════

#: The five stages plan.md Phase 2 step 4 requires be measured *separately*. One end-to-end
#: number cannot distinguish "the model is slow" from "our queue is deep" from "the endpointer
#: waits 800 ms by design", and those have completely different remedies.
LATENCY_STAGES: tuple[str, ...] = (
    "algorithmic",  # inherent to the chunking: a 640 ms chunk cannot answer before 640 ms
    "queueing",     # time spent in our own bounded queues, both directions
    "inference",    # the backend's own compute
    "endpoint",     # the VAD's endpoint-silence wait before finalisation (~0.8 s by design)
    "ui_commit",    # stability dwell + cadence flush + injection, i.e. our own commit path
)


@dataclass(frozen=True)
class LatencySample:
    """One repeat of one measurement, decomposed.

    `measured_end_to_end_ms` is the independently observed figure when there is one. It is
    optional because some stages are instrumented before an end-to-end clock exists, but when it
    is present :attr:`residual_ms` is the honest statement of how much of the wall time the
    decomposition failed to account for. A decomposition that always sums exactly to a separately
    measured total is usually a decomposition computed *from* the total.
    """

    algorithmic_ms: float = 0.0
    queueing_ms: float = 0.0
    inference_ms: float = 0.0
    endpoint_ms: float = 0.0
    ui_commit_ms: float = 0.0
    measured_end_to_end_ms: float | None = None
    cold: bool = False
    label: str = ""

    @property
    def stages(self) -> dict[str, float]:
        return {
            "algorithmic": self.algorithmic_ms,
            "queueing": self.queueing_ms,
            "inference": self.inference_ms,
            "endpoint": self.endpoint_ms,
            "ui_commit": self.ui_commit_ms,
        }

    @property
    def component_sum_ms(self) -> float:
        return sum(self.stages.values())

    @property
    def residual_ms(self) -> float | None:
        if self.measured_end_to_end_ms is None:
            return None
        return self.measured_end_to_end_ms - self.component_sum_ms

    def is_consistent(self, tolerance_ms: float = 1.0) -> bool:
        r = self.residual_ms
        return True if r is None else abs(r) <= tolerance_ms


@dataclass(frozen=True)
class Spread:
    """Median and spread over repeats. plan.md Phase 2 step 4 asks for run-to-run variance, so a
    single sample is never reported as if it were the answer."""

    n: int
    median: float
    p25: float
    p75: float
    minimum: float
    maximum: float
    mad: float          # median absolute deviation — robust, unlike stdev, to one slow cold run
    mean: float
    stdev: float | None  # None for n < 2

    @property
    def iqr(self) -> float:
        return self.p75 - self.p25

    def to_dict(self) -> dict:
        return {
            "n": self.n, "median": self.median, "p25": self.p25, "p75": self.p75,
            "iqr": self.iqr, "min": self.minimum, "max": self.maximum,
            "mad": self.mad, "mean": self.mean, "stdev": self.stdev,
        }


def _percentile(values: Sequence[float], q: float) -> float:
    """Linear-interpolation percentile. Written out rather than using ``statistics.quantiles``
    because that needs n >= 2 and a benchmark cell legitimately has one repeat while it is being
    brought up."""
    xs = sorted(values)
    if not xs:
        raise ValueError("percentile of an empty sample")
    if len(xs) == 1:
        return float(xs[0])
    pos = q * (len(xs) - 1)
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return float(xs[lo])
    return float(xs[lo] + (xs[hi] - xs[lo]) * (pos - lo))


def _spread(values: Sequence[float]) -> Spread:
    xs = [float(v) for v in values]
    med = statistics.median(xs)
    return Spread(
        n=len(xs),
        median=med,
        p25=_percentile(xs, 0.25),
        p75=_percentile(xs, 0.75),
        minimum=min(xs),
        maximum=max(xs),
        mad=statistics.median([abs(x - med) for x in xs]),
        mean=statistics.fmean(xs),
        stdev=statistics.stdev(xs) if len(xs) > 1 else None,
    )


@dataclass(frozen=True)
class LatencyReport:
    stage_spreads: dict[str, Spread]
    total_spread: Spread
    measured_spread: Spread | None
    n: int
    cold: bool
    inconsistent_samples: tuple[str, ...]

    def to_dict(self) -> dict:
        return {
            "cold": self.cold,
            "n": self.n,
            "stages": {k: v.to_dict() for k, v in self.stage_spreads.items()},
            "component_sum": self.total_spread.to_dict(),
            "measured_end_to_end": (self.measured_spread.to_dict()
                                    if self.measured_spread else None),
            "inconsistent_samples": list(self.inconsistent_samples),
            "note": ("stages are measured separately and reported separately; the component sum "
                     "is shown next to the independently measured end-to-end figure so a gap "
                     "between them is visible rather than absorbed"),
        }


def summarise_latency(samples: Sequence[LatencySample], *,
                      tolerance_ms: float = 1.0) -> dict[str, LatencyReport]:
    """Group samples into ``{"cold": …, "warm": …}`` and summarise each stage over repeats.

    Cold and warm are never pooled. plan.md asks for both because a cold first run includes model
    load and kernel autotuning, and averaging one cold run into nine warm ones produces a number
    that describes no run that ever happened.
    """
    out: dict[str, LatencyReport] = {}
    for key, cold in (("cold", True), ("warm", False)):
        group = [s for s in samples if s.cold is cold]
        if not group:
            continue
        stage_spreads = {
            stage: _spread([s.stages[stage] for s in group]) for stage in LATENCY_STAGES
        }
        measured = [s.measured_end_to_end_ms for s in group if s.measured_end_to_end_ms is not None]
        out[key] = LatencyReport(
            stage_spreads=stage_spreads,
            total_spread=_spread([s.component_sum_ms for s in group]),
            measured_spread=_spread(measured) if measured else None,
            n=len(group),
            cold=cold,
            inconsistent_samples=tuple(
                s.label or f"sample[{i}]"
                for i, s in enumerate(group) if not s.is_consistent(tolerance_ms)
            ),
        )
    return out


def real_time_factor(audio_duration_s: float, processing_wall_s: float) -> float:
    """RTF = processing wall time / audio duration. Below 1.0 is faster than real time.

    Wall time, not CPU time, and not GPU kernel time: a backend that is fast on the GPU and slow
    at getting data to it is slow. Raises on a non-positive duration rather than returning inf,
    because an inf in a result cell propagates into every aggregate that touches it.
    """
    if audio_duration_s <= 0:
        raise ValueError("audio_duration_s must be positive")
    return processing_wall_s / audio_duration_s


@dataclass(frozen=True)
class ResourceReport:
    """Peak VRAM and start-up cost. VRAM gates in plan.md §4; start-up is reported, not gated.

    `peak_vram_mb` is the peak *this process* held, measured by the runner — this module does not
    query the GPU, because doing so would put a CUDA or NVML dependency into a stdlib-only file.
    """

    peak_vram_mb: float | None = None
    cold_start_ms: float | None = None
    warm_start_ms: float | None = None
    source: str = "reported-by-runner"

    def to_dict(self) -> dict:
        return {
            "peak_vram_mb": self.peak_vram_mb,
            "cold_start_ms": self.cold_start_ms,
            "warm_start_ms": self.warm_start_ms,
            "source": self.source,
        }


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 8. Hallucination on silence-only and noise-only input
# ══════════════════════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class HallucinationCase:
    """One silence-only or noise-only clip and what the backend said about it."""

    clip_id: str
    kind: str          # "silence" | "noise"
    output_text: str
    duration_s: float | None = None


@dataclass(frozen=True)
class HallucinationReport:
    total: int
    hallucinated: int
    by_kind: dict[str, tuple[int, int]] = field(default_factory=dict)  # kind -> (halluc, total)
    outputs: tuple[tuple[str, str, str], ...] = ()  # (clip_id, kind, text) for hallucinations

    @property
    def rate(self) -> float:
        return self.hallucinated / self.total if self.total else 0.0

    def to_dict(self, *, include_text: bool = True) -> dict:
        out: dict = {
            "total": self.total,
            "hallucinated": self.hallucinated,
            "rate": self.rate,
            "by_kind": {k: {"hallucinated": h, "total": t, "rate": (h / t if t else 0.0)}
                        for k, (h, t) in sorted(self.by_kind.items())},
            "criterion": ("any non-whitespace output on a silence-only or noise-only clip. Not "
                          "normalised first: a lone '.' or 'Thank you.' is exactly the failure "
                          "plan.md §1.6 cites, and normalising before the test would erase the "
                          "punctuation-only case"),
        }
        if include_text:
            out["texts"] = [{"clip_id": c, "kind": k, "text": t} for c, k, t in self.outputs]
            out["contains_transcript_text"] = True
        return out


def is_hallucination(reference: str, hypothesis: str) -> bool:
    """True when the reference is empty (silence or noise) and the hypothesis is not.

    The emptiness test is ``strip()``, on the raw text. Deliberately not the normaliser: the
    normaliser strips edge punctuation, so a hypothesis of "." would normalise to "" and a real
    hallucination would be scored as silence correctly recognised.
    """
    return reference.strip() == "" and hypothesis.strip() != ""


def hallucination_report(cases: Sequence[HallucinationCase]) -> HallucinationReport:
    """Rate and the verbatim text, per plan.md §4.

    The text is reported because it is diagnostic and cheap: "Thank you." over trailing silence is
    a known Whisper behaviour (plan.md §1.6, citing wan-dancer's own ``notes/transcribe.py``), and
    seeing it is what tells you the fault is the model rather than the VAD gate. It also means
    the output carries transcript text, so ``contains_transcript_text`` is set and plan.md §1.9's
    gitignore rule applies to any file holding it.
    """
    by_kind: dict[str, list[int]] = {}
    outputs: list[tuple[str, str, str]] = []
    hallucinated = 0
    for case in cases:
        bucket = by_kind.setdefault(case.kind, [0, 0])
        bucket[1] += 1
        if case.output_text.strip() != "":
            hallucinated += 1
            bucket[0] += 1
            outputs.append((case.clip_id, case.kind, case.output_text))
    return HallucinationReport(
        total=len(cases),
        hallucinated=hallucinated,
        by_kind={k: (v[0], v[1]) for k, v in by_kind.items()},
        outputs=tuple(outputs),
    )


# ══════════════════════════════════════════════════════════════════════════════════════════════
# 9. What the selection rule is allowed to see
# ══════════════════════════════════════════════════════════════════════════════════════════════


def select_gate_metrics() -> tuple[str, ...]:
    """The metric names plan.md §4's pre-registered rule may use, in the order it uses them.

    ``run_matrix.py`` reads this instead of naming metrics itself, so "flicker gates" cannot be
    introduced by an edit in the runner. Objective: maximise held-out technical-term F1 among
    configurations that meet the first-partial, finalisation, ICER and VRAM limits; ties break on
    ICER, then latency.
    """
    return (
        "term_f1",                # objective, maximised
        "first_partial_ms",       # limit
        "finalisation_ms",        # limit
        "icer_per_1000",          # limit, then first tie-break
        "peak_vram_mb",           # limit
        "latency_median_ms",      # second tie-break
    )


def metric_versions() -> dict[str, str]:
    """Every ruleset version, for embedding in each result cell.

    A number produced under a different normaliser, term table or grapheme approximation is not
    comparable to one produced under this one. Recording the versions is what turns that from an
    invisible problem into a visible mismatch.
    """
    return {
        "metrics": METRICS_VERSION,
        "normaliser": NORMALISER_VERSION,
        "terms": TERM_RULESET_VERSION,
        "grapheme": GRAPHEME_RULESET_VERSION,
    }
