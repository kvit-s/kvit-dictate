"""Tests for :mod:`kvit_asr.stability` — plan.md §1.4 and docs/protocol.md §8.2.

Run with::

    python -m unittest discover -s server/tests

stdlib ``unittest`` only, no pytest, no third-party anything.

The property most of these tests assert is not "the algorithm produced value X" but "the algorithm
never produced a value the Client would have to reject". docs/protocol.md §8.3 makes a violation
unrecoverable *for the document* — the Client preserves whatever it already typed and never
repairs — so the interesting assertions are the negative ones, checked over whole hypothesis
streams rather than at single points.
"""

import os
import random
import sys
import unittest

# Make ``server/`` importable regardless of where the runner was started from. ``unittest
# discover -s server/tests`` puts ``server/tests`` on sys.path, not ``server``, and the repository
# root is the natural cwd — so without this, the package is only importable by accident.
_SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from kvit_asr.stability import (  # noqa: E402
    ManualClock,
    StabilityParams,
    StabilityTracker,
    StabilityViolation,
    ViolationKind,
    common_grapheme_prefix,
    count_graphemes,
    grapheme_boundaries,
    is_grapheme_boundary,
    retract_graphemes,
    retract_to_word_boundary,
    trim_trailing_whitespace,
    utf8_len,
)


# ─── Helpers ────────────────────────────────────────────────────────────────────────────────────


def run_stream(tracker, texts, clock, step_ms=1000):
    """Feed a hypothesis stream, advancing the clock between hypotheses.

    Returns the list of ``(text, stable_bytes)`` the server would have put on the wire.

    Models a server that only ever evaluates on a *new hypothesis* — i.e. one that never calls
    ``Session.tick()``. That is deliberately the weaker of the two drive patterns, because it is
    the one where a candidate that keeps growing never gets its T milliseconds of quiet; see
    :func:`run_stream_ticked` for the other.
    """
    out = []
    for t in texts:
        clock.advance(step_ms)
        out.append((t, tracker.observe(t)))
    return out


def mature(tracker, clock, extra_ms=None):
    """Let plan.md §1.4 step 5's T-millisecond hold expire, then re-evaluate. Returns stable_bytes.

    ─── Why this exists, because its absence looked exactly like a bug in the tracker ───────────

    Step 5 requires the candidate "to have been unchanged for T ms". With ``N = 2`` the candidate
    is *pinned at zero* until the window holds two hypotheses — not because the prefix is unstable
    but because nothing has been confirmed by a second opinion yet — so the first real candidate
    comes into existence at the instant the second hypothesis lands. At that instant it has been
    unchanged for 0 ms, not T, and :meth:`StabilityTracker.observe` correctly publishes nothing.

    So ``run_stream(t, [a, b], clock)`` ends with ``high_water == 0`` **however far the clock was
    advanced between a and b** — the advance happens before the candidate exists, so it buys
    nothing. Four tests in an earlier revision of this file read that as the tracker failing to
    advance its high-water mark. It is not; it is this file having driven the tracker through
    only half of its interface. The dwell is a *clock* condition and the clock-driven entry point
    is :meth:`StabilityTracker.poll`, which ``Session.tick()`` exists to call.

    That makes this helper the test-side twin of ``Session.tick()``, and using it here is the
    same thing the production loop must do. It is not a workaround: a server that never ticks
    genuinely cannot publish anything for a hypothesis stream whose candidate moves on every
    hypothesis, which is worth knowing and is reported alongside these tests.
    """
    clock.advance(tracker.params.hold_ms_t if extra_ms is None else extra_ms)
    return tracker.poll()


def run_stream_ticked(tracker, texts, clock, step_ms=1000):
    """Feed a hypothesis stream the way :class:`kvit_asr.session.Session` drives the tracker.

    ``observe`` on each hypothesis, then a ``tick`` once the hold could have matured, emitting a
    second message only when ``stable_bytes`` actually advanced — which is precisely
    ``Session.tick()``'s "an identical re-send would consume a seq to say nothing" rule. The
    returned list is therefore the real wire stream, interleaved, and every wire invariant has to
    survive the interleaving and not merely each entry point on its own.
    """
    out = []
    last = 0
    for t in texts:
        clock.advance(step_ms)
        stable = tracker.observe(t)
        out.append((t, stable))
        last = stable
        clock.advance(tracker.params.hold_ms_t)
        polled = tracker.poll()
        if polled > last:
            out.append((t, polled))
            last = polled
    return out


class StabilityAssertions(unittest.TestCase):
    """Mixin: the four wire invariants, asserted over a whole emitted stream."""

    def assert_stream_conforms(self, emitted, final_text=None):
        """The four §8.2 obligations, checked against the *cumulative* published prefix.

        Deliberately not the pairwise check against the immediately preceding message. Pairwise
        plus monotonicity does imply the cumulative property by induction, but the induction is
        the kind of argument that quietly stops holding when someone adds a case — and the
        cumulative form is what docs/protocol.md §8.2 actually says and what the Client actually
        relies on, since the Client compares against what it *typed*, not against the last frame.
        """
        high_water = 0
        published = b""            # the exact bytes claimed stable so far, not a length
        for text, stable in emitted:
            enc = text.encode("utf-8")

            # §8.2 requirement 1 — range.
            self.assertGreaterEqual(stable, 0)
            self.assertLessEqual(stable, len(enc))

            # §8.2 requirement 2 — grapheme-cluster boundary.
            self.assertTrue(
                is_grapheme_boundary(text, stable),
                f"stable_bytes={stable} is not a cluster boundary of {text!r}",
            )

            # §8.2 requirement 3, first half — monotonic within the utterance.
            self.assertGreaterEqual(stable, high_water, f"stable_bytes decreased at {text!r}")

            # §8.2 requirement 3, second half — the one with teeth. Every byte ever published as
            # stable must still be there, byte-identical, in this hypothesis. A Client that typed
            # those bytes into the user's document cannot take them back.
            self.assertEqual(
                enc[:high_water], published,
                f"published prefix mutated: {published!r} is no longer a prefix of {text!r}",
            )

            high_water, published = stable, enc[:stable]

        if final_text is not None:
            # §8.2's last sentence: utf8(final.text) MUST begin with the highest stable prefix
            # published for the utterance.
            self.assertEqual(
                final_text.encode("utf-8")[:high_water], published,
                f"final {final_text!r} does not begin with the published prefix {published!r}",
            )

    def assert_never_partial_word(self, emitted):
        """The stable prefix always ends at a word end, with trailing whitespace held back."""
        for text, stable in emitted:
            if stable == 0:
                continue
            prefix = text.encode("utf-8")[:stable].decode("utf-8")
            self.assertFalse(
                prefix[-1:].isspace(),
                f"trailing whitespace was committed in {prefix!r}",
            )
            if len(prefix) < len(text):
                nxt = text[len(prefix)]
                self.assertTrue(
                    nxt.isspace(),
                    f"stable prefix {prefix!r} stops mid-word before {nxt!r} in {text!r}",
                )


# ─── Grapheme / offset primitives ───────────────────────────────────────────────────────────────


class TestGraphemePrimitives(unittest.TestCase):

    def test_offsets_are_utf8_bytes_not_code_points(self):
        text = "señor"                    # 5 code points, 6 UTF-8 bytes
        self.assertEqual(len(text), 5)
        self.assertEqual(utf8_len(text), 6)
        self.assertEqual(count_graphemes(text), 5)
        # The boundary after "señ" is byte 4, not code point 3.
        self.assertIn(4, grapheme_boundaries(text))
        self.assertFalse(is_grapheme_boundary(text, 3))   # splits the 2-byte ñ

    def test_combining_sequence_is_one_cluster(self):
        text = "café"                    # 5 code points, 6 bytes, 4 clusters
        self.assertEqual(utf8_len(text), 6)
        self.assertEqual(count_graphemes(text), 4)
        self.assertEqual(grapheme_boundaries(text), (0, 1, 2, 3, 6))
        # Byte 4 is a valid UTF-8 boundary and an invalid cluster boundary — the exact shape of
        # fixture 013.
        self.assertFalse(is_grapheme_boundary(text, 4))

    def test_zwj_emoji_is_one_cluster(self):
        fam = "\U0001F468‍\U0001F469‍\U0001F467"
        self.assertEqual(count_graphemes(fam), 1)
        self.assertEqual(grapheme_boundaries(fam), (0, utf8_len(fam)))

    def test_regional_indicator_pairs(self):
        flags = "\U0001F1E9\U0001F1EA\U0001F1EB\U0001F1F7"   # 🇩🇪🇫🇷
        self.assertEqual(count_graphemes(flags), 2)
        self.assertEqual(grapheme_boundaries(flags), (0, 8, 16))

    def test_crlf_is_one_cluster(self):
        self.assertEqual(count_graphemes("a\r\nb"), 3)

    def test_keycap_and_variation_selector(self):
        keycap = "1️⃣"
        self.assertEqual(count_graphemes(keycap), 1)

    def test_retract_to_word_boundary_worked_examples(self):
        # These five are copied verbatim from src/util/utf8.h, which states them as the unit tests
        # for the C++ twin. If either side changes, both must.
        self.assertEqual(retract_to_word_boundary("hello world", 11), 5)
        self.assertEqual(retract_to_word_boundary("hello world", 5), 0)
        self.assertEqual(retract_to_word_boundary("Hi. There", 9), 3)
        self.assertEqual(retract_to_word_boundary("hello", 5), 0)
        self.assertEqual(retract_to_word_boundary("hello ", 6), 5)

    def test_inner_dot_is_not_a_word_end(self):
        # plan.md §4's motivating example: "llama.cpp" must not be committed as "llama.".
        self.assertEqual(retract_to_word_boundary("use llama.cpp now", 13), 3)

    def test_trim_trailing_whitespace(self):
        self.assertEqual(trim_trailing_whitespace("hello   ", 8), 5)
        self.assertEqual(trim_trailing_whitespace("hello", 5), 5)
        self.assertEqual(trim_trailing_whitespace("   ", 3), 0)

    def test_retract_graphemes_counts_clusters_not_bytes(self):
        text = "abcafé"                  # clusters: a b c a f é  -> 6 clusters, 8 bytes
        self.assertEqual(count_graphemes(text), 6)
        # Retracting 1 cluster from the end removes the whole 3-byte "é" cluster.
        self.assertEqual(retract_graphemes(text, utf8_len(text), 1), 5)
        self.assertEqual(retract_graphemes(text, utf8_len(text), 99), 0)


class TestCommonGraphemePrefix(unittest.TestCase):

    def test_divergence_inside_a_combining_sequence(self):
        a = "un café noir"
        b = "un cafe noir"
        # A byte-wise prefix would be 7 ("un cafe"), which lands *inside* a's "é" cluster and
        # would type a base letter whose accent the model is still revising.
        self.assertEqual(a.encode("utf-8")[:7], b.encode("utf-8")[:7])
        self.assertFalse(is_grapheme_boundary(a, 7))
        self.assertEqual(common_grapheme_prefix(a, b), 6)
        self.assertTrue(is_grapheme_boundary(a, 6))

    def test_divergence_inside_an_emoji_zwj_sequence(self):
        a = "family \U0001F468‍\U0001F469‍\U0001F467 here"
        b = "family \U0001F468‍\U0001F469 here"
        # The raw byte prefix runs to 18 — through 👨 ZWJ 👩 — which is inside a's single cluster.
        self.assertEqual(a.encode("utf-8")[:18], b.encode("utf-8")[:18])
        self.assertFalse(is_grapheme_boundary(a, 18))
        self.assertEqual(common_grapheme_prefix(a, b), 7)
        self.assertTrue(is_grapheme_boundary(a, 7))

    def test_identical_and_empty(self):
        self.assertEqual(common_grapheme_prefix("abc", "abc"), 3)
        self.assertEqual(common_grapheme_prefix("", "abc"), 0)
        self.assertEqual(common_grapheme_prefix("abc", ""), 0)


# ─── The tracker ────────────────────────────────────────────────────────────────────────────────


class TestStabilityTracker(StabilityAssertions):

    def make(self, **kw):
        clock = ManualClock(0)
        params = StabilityParams(**kw) if kw else StabilityParams()
        t = StabilityTracker(params, clock=clock)
        t.reset(utterance_id=1)
        return t, clock

    # ── Growth over a realistic revision sequence ───────────────────────────────────────────────

    def test_monotonic_growth_over_realistic_revisions(self):
        # A technical-dictation stream of the shape plan.md §4 cares about, including the
        # "llama dot c p p" -> "llama.cpp" rewrite that is the whole reason raw WER is reported
        # alongside normalised WER.
        texts = [
            "can",
            "can you",
            "can you change",
            "can you change the",
            "can you change the llama",
            "can you change the llama dot",
            "can you change the llama dot c p p",
            "can you change the llama.cpp",
            "can you change the llama.cpp config",
            "can you change the llama.cpp configuration",
        ]
        t, clock = self.make()
        emitted = run_stream(t, texts, clock)

        self.assert_stream_conforms(emitted)
        self.assert_never_partial_word(emitted)

        # It must actually make progress, or the test would pass on a tracker that returns 0.
        self.assertGreater(emitted[-1][1], 0)
        final_prefix = texts[-1].encode("utf-8")[: emitted[-1][1]].decode("utf-8")
        self.assertTrue(texts[-1].startswith(final_prefix))
        self.assertIn("can you change the", final_prefix)

    def test_tail_revises_repeatedly_and_nothing_published_ever_changes(self):
        # The tail is where revision concentrates (plan.md §1.4 step 3). This stream rewrites the
        # last two or three words on almost every hypothesis while the head stays put.
        head = "set the temperature to zero point"
        tails = [
            "seven",
            "seven five",
            "seventy five",
            "seven point five",
            "seventy-five",
            "seven five percent",
            "seventy five percent",
        ]
        texts = [f"{head} {tail}" for tail in tails]
        t, clock = self.make()
        emitted = run_stream(t, texts, clock)

        self.assert_stream_conforms(emitted)
        self.assert_never_partial_word(emitted)

        # Stronger than pairwise: everything ever published must still be a prefix of every later
        # hypothesis. That is plan.md §1.4's invariant stated directly.
        high_water = 0
        published = b""
        for text, stable in emitted:
            enc = text.encode("utf-8")
            self.assertEqual(enc[:high_water], published)
            high_water, published = stable, enc[:stable]

    def test_partial_words_are_never_marked_stable(self):
        # Hypotheses that agree on a word *stem* but not on its ending. Committing the stem is the
        # single most common way to corrupt a document, because a revised word ending is the most
        # common revision there is.
        texts = [
            "the configuration file is corrupt",
            "the configurable file is corrupt",
            "the configuration files are corrupt",
            "the configuration file is corrupted",
        ]
        t, clock = self.make()
        emitted = run_stream(t, texts, clock)

        self.assert_stream_conforms(emitted)
        self.assert_never_partial_word(emitted)
        for text, stable in emitted:
            prefix = text.encode("utf-8")[:stable].decode("utf-8")
            self.assertNotIn(prefix, ("the configur", "the configuration file is"))
            self.assertFalse(prefix.endswith("configur"))

    def test_trailing_whitespace_is_held_back(self):
        texts = [
            "restart the service now",
            "restart the service now please",
            "restart the service now, please",
        ]
        t, clock = self.make()
        emitted = run_stream(t, texts, clock)
        self.assert_stream_conforms(emitted)
        for text, stable in emitted:
            prefix = text.encode("utf-8")[:stable].decode("utf-8")
            self.assertFalse(prefix.endswith(" "), f"committed a trailing space in {prefix!r}")
            self.assertEqual(prefix, prefix.rstrip())

    def test_trailing_whitespace_in_the_hypothesis_itself(self):
        # A backend that emits a trailing space on every hypothesis must not cause the space to be
        # published, even though it is stable by every other measure.
        texts = ["alpha beta gamma delta "] * 3
        t, clock = self.make()
        emitted = run_stream(t, texts, clock)
        self.assert_stream_conforms(emitted)
        for text, stable in emitted:
            self.assertFalse(text.encode("utf-8")[:stable].decode("utf-8").endswith(" "))

    # ── Utterance boundary ──────────────────────────────────────────────────────────────────────

    def test_reset_at_utterance_boundary_clears_everything(self):
        t, clock = self.make()
        run_stream(t, [
            "the first utterance runs on for a while",
            "the first utterance runs on for a good while",
        ], clock)
        # Two hypotheses give the candidate; the hold gives it eligibility. See mature().
        self.assertGreater(mature(t, clock), 0)
        self.assertGreater(t.high_water, 0)
        self.assertTrue(t.stable_prefix)

        t.reset(utterance_id=2)
        self.assertEqual(t.utterance_id, 2)
        self.assertEqual(t.high_water, 0)
        self.assertEqual(t.stable_prefix, b"")
        self.assertIsNone(t.last_text)

        # A completely unrelated second utterance must now be accepted without a violation — the
        # high-water mark of utterance 1 constrains nothing in utterance 2 (docs §8.2, last para).
        emitted = run_stream(t, [
            "nothing here resembles the previous sentence",
            "nothing here resembles the previous sentence at all",
        ], clock)
        self.assert_stream_conforms(emitted)

    def test_finalise_requires_the_published_prefix(self):
        t, clock = self.make()
        run_stream(t, [
            "open the settings dialog and change",
            "open the settings dialog and change it",
        ], clock)
        mature(t, clock)
        self.assertGreater(t.high_water, 0)

        text = "open the settings dialog and change it."
        self.assertEqual(t.finalise(text), utf8_len(text))
        self.assertEqual(t.high_water, utf8_len(text))

        t.reset(utterance_id=2)
        run_stream(t, [
            "open the settings dialog and change",
            "open the settings dialog and change it",
        ], clock)
        # Without maturing, high_water would be 0 and *any* final would be accepted — the
        # assertion below would then be testing nothing.
        self.assertGreater(mature(t, clock), 0)
        with self.assertRaises(StabilityViolation) as cm:
            t.finalise("close the settings dialog and change it.")
        self.assertEqual(cm.exception.kind, ViolationKind.STABLE_PREFIX_MUTATED)

    # ── Unicode boundaries under revision ───────────────────────────────────────────────────────

    def test_divergence_inside_a_combining_sequence_is_never_published(self):
        # The byte-wise common prefix of these two is 7, inside the "é" cluster.
        texts = [
            "un café noir et une biere",
            "un cafe noir et une biere",
            "un café noir et une bière",
        ]
        t, clock = self.make()
        emitted = run_stream(t, texts, clock)
        self.assert_stream_conforms(emitted)
        for text, stable in emitted:
            self.assertNotEqual(stable, 7)
            self.assertTrue(is_grapheme_boundary(text, stable))

    def test_divergence_inside_an_emoji_zwj_sequence_is_never_published(self):
        a = "send \U0001F468‍\U0001F469‍\U0001F467 to the group chat"
        b = "send \U0001F468‍\U0001F469 to the group chat"
        t, clock = self.make()
        emitted = run_stream(t, [a, b, a], clock)
        self.assert_stream_conforms(emitted)
        for text, stable in emitted:
            self.assertTrue(is_grapheme_boundary(text, stable))
            # The family cluster starts at byte 5; nothing inside it may be published.
            self.assertTrue(stable <= 5 or stable >= 5 + utf8_len(
                "\U0001F468‍\U0001F469‍\U0001F467"))

    # ── Step 5, the T-millisecond hold ──────────────────────────────────────────────────────────

    def test_hold_time_with_injected_clock(self):
        t, clock = self.make(hypotheses_n=2, retract_clusters_m=3, hold_ms_t=200)
        text = "alpha beta gamma delta epsilon"

        self.assertEqual(t.observe(text), 0)      # one hypothesis: nothing confirmed yet
        self.assertEqual(t.observe(text), 0)      # candidate appears, dwell starts at t=0

        clock.advance(199)
        self.assertEqual(t.poll(), 0, "published before the hold expired")

        clock.advance(1)
        matured = t.poll()
        self.assertGreater(matured, 0, "the hold expired and nothing was published")
        self.assertEqual(text.encode("utf-8")[:matured].decode("utf-8"), "alpha beta gamma delta")

    def test_changing_candidate_restarts_the_hold(self):
        t, clock = self.make(hold_ms_t=200)
        t.observe("alpha beta gamma delta epsilon")
        t.observe("alpha beta gamma delta epsilon")

        clock.advance(150)
        # A new hypothesis that moves the candidate must restart the dwell, not inherit it.
        t.observe("alpha beta gamma DELTA epsilon zeta")
        clock.advance(100)                        # 250 ms since the first candidate, 100 since this
        self.assertEqual(t.poll(), 0)

        clock.advance(100)
        self.assertGreater(t.poll(), 0)

    def test_poll_is_not_a_second_opinion(self):
        # N hypotheses means N *hypotheses*. Polling the same text repeatedly must not fill the
        # window and make a single guess look confirmed.
        t, clock = self.make(hypotheses_n=2, hold_ms_t=0)
        self.assertEqual(t.observe("alpha beta gamma delta epsilon"), 0)
        for _ in range(10):
            clock.advance(1000)
            self.assertEqual(t.poll(), 0)

    def test_zero_hold_publishes_immediately(self):
        t, clock = self.make(hold_ms_t=0)
        t.observe("alpha beta gamma delta epsilon")
        self.assertGreater(t.observe("alpha beta gamma delta epsilon"), 0)

    # ── The internal assertion ──────────────────────────────────────────────────────────────────

    def test_internal_check_fires_on_a_synthetic_backwards_step(self):
        t, clock = self.make()
        text = "the quick brown fox jumps over the lazy dog"
        run_stream(t, [text, text], clock)
        mature(t, clock)
        self.assertGreater(t.high_water, 0)

        with self.assertRaises(StabilityViolation) as cm:
            t.check_emission(text, t.high_water - 1)
        v = cm.exception
        self.assertEqual(v.kind, ViolationKind.STABLE_BYTES_DECREASED)
        self.assertEqual(v.previous_stable_bytes, t.high_water)
        self.assertEqual(v.incoming_stable_bytes, t.high_water - 1)

        # The tracker's own state must be untouched by the rejected emission.
        self.assertEqual(t.high_water, len(t.stable_prefix))

    def test_internal_check_fires_on_out_of_range(self):
        t, _ = self.make()
        with self.assertRaises(StabilityViolation) as cm:
            t.check_emission("abc", 4)
        self.assertEqual(cm.exception.kind, ViolationKind.STABLE_BYTES_OUT_OF_RANGE)

    def test_internal_check_fires_off_a_cluster_boundary(self):
        # Byte 4 of "café" is a valid UTF-8 boundary and an invalid cluster boundary.
        t, _ = self.make()
        with self.assertRaises(StabilityViolation) as cm:
            t.check_emission("café", 4)
        self.assertEqual(cm.exception.kind, ViolationKind.STABLE_BYTES_NOT_ON_BOUNDARY)

    def test_backend_retraction_raises_rather_than_being_clamped(self):
        # A backend that rewrites text below the published mark. docs/protocol.md §8.3 forbids
        # clamping; there is no conforming stable_bytes for this message, so it cannot be sent.
        t, clock = self.make()
        run_stream(t, [
            "the quick brown fox jumps",
            "the quick brown fox jumps over",
        ], clock)
        # There is nothing to clamp until something has been published, so this test is vacuous
        # without the hold expiring first.
        self.assertGreater(mature(t, clock), 0)

        clock.advance(1000)
        with self.assertRaises(StabilityViolation) as cm:
            t.observe("the quick brown cat jumps over")
        v = cm.exception
        self.assertEqual(v.kind, ViolationKind.STABLE_PREFIX_MUTATED)
        self.assertEqual(v.diverge_offset, 16)   # "the quick brown " is 16 bytes

    # ── Parameter sweep, since plan.md §4 sweeps them ───────────────────────────────────────────

    def test_all_swept_parameter_combinations_stay_conforming(self):
        texts = [
            "run the benchmark",
            "run the benchmark on",
            "run the benchmark on the held out set",
            "run the benchmark on the held-out set",
            "run the benchmark on the held-out set twice",
            "run the benchmark on the heldout set twice",
        ]
        for n in (2, 3):
            for m in (2, 3, 5):
                for hold in (100, 200, 400):
                    with self.subTest(N=n, M=m, T=hold):
                        clock = ManualClock(0)
                        t = StabilityTracker(
                            StabilityParams(hypotheses_n=n, retract_clusters_m=m, hold_ms_t=hold),
                            clock=clock,
                        )
                        t.reset(utterance_id=1)
                        emitted = run_stream(t, texts, clock)
                        self.assert_stream_conforms(emitted)
                        self.assert_never_partial_word(emitted)

    def test_m_cluster_retraction_is_what_saves_this_stream(self):
        """Step 3 is load-bearing, demonstrated rather than asserted about.

        Two identical hypotheses agree on a complete word *followed by a space*, which defeats
        step 4 — the word boundary is real, so "seven" looks committable. The third hypothesis
        revises it to "several". Step 3's retraction to M clusters behind the decode head is the
        only thing standing between that and a published prefix the next hypothesis contradicts.

        Verified by mutation: with ``retract_graphemes`` neutered to the identity, this stream
        publishes 22 bytes ("…to seven") on the second hypothesis and the third raises
        ``stable_prefix_mutated``. With M = 3 it publishes 16 ("…to") and the third is accepted.
        """
        texts = [
            "set the value to seven ",
            "set the value to seven ",
            "set the value to several ",
        ]
        t, clock = self.make()
        emitted = run_stream_ticked(t, texts, clock, step_ms=120)

        self.assert_stream_conforms(emitted)
        self.assert_never_partial_word(emitted)
        # "seven" must never have been published — it is inside the revision zone.
        for text, stable in emitted:
            self.assertLessEqual(stable, len("set the value to"))
        self.assertEqual(t.high_water, len("set the value to"))

    # ── The two drive patterns, and the invariant across both ───────────────────────────────────

    def test_ticked_drive_publishes_what_the_hypothesis_only_drive_withholds(self):
        """``Session.tick()``'s reason for existing, stated as a comparison.

        The same hypothesis stream, driven once with ``observe`` alone and once the way the
        session loop drives it. Both must conform; the ticked one must get further, because the
        T-millisecond hold is a clock condition and only ``poll`` advances a clock condition.
        """
        texts = [
            "deploy the new",
            "deploy the new build",
            "deploy the new build to staging",
            "deploy the new build to staging first",
        ]

        c1 = ManualClock(0)
        t1 = StabilityTracker(StabilityParams(), clock=c1)
        t1.reset(utterance_id=1)
        untickled = run_stream(t1, texts, c1)

        c2 = ManualClock(0)
        t2 = StabilityTracker(StabilityParams(), clock=c2)
        t2.reset(utterance_id=1)
        ticked = run_stream_ticked(t2, texts, c2)

        self.assert_stream_conforms(untickled)
        self.assert_never_partial_word(untickled)
        self.assert_stream_conforms(ticked, final_text=texts[-1])
        self.assert_never_partial_word(ticked)

        self.assertGreater(t2.high_water, t1.high_water)
        self.assertEqual(
            texts[-1].encode("utf-8")[: t2.high_water].decode("utf-8"),
            "deploy the new build to",
        )

    def test_invariant_holds_over_many_generated_tail_revision_streams(self):
        """The invariant with teeth, over 200 streams rather than four hand-written ones.

        Every hypothesis is a genuine prefix of one fixed sentence plus a tail token that is
        unique to that hypothesis, which is what a streaming decoder actually does: the head
        settles, the tail is a guess that is thrown away. Because no two hypotheses ever agree on
        a tail token, nothing garbled can survive N-hypothesis agreement — so *any*
        ``StabilityViolation`` raised here is a defect in the tracker, not in the stream, and is
        allowed to propagate as a test error rather than being caught.

        Deterministic: a fixed seed, so a failure is reproducible from the test name alone.
        """
        rng = random.Random(20260816)
        words = ("open", "the", "kvit", "notes", "window", "and", "search", "for",
                 "the", "café", "réunion", "notes", "from", "last", "Tuesday")

        for trial in range(200):
            sentence = " ".join(words[: rng.randrange(4, len(words) + 1)])
            true_words = sentence.split(" ")

            texts = []
            for i in range(1, len(true_words) + 1):
                head = " ".join(true_words[:i])
                # A unique garbled tail, so it can never be confirmed by a second hypothesis.
                texts.append(f"{head} {trial}x{i}" if rng.random() < 0.7 else head)
                texts.append(head)          # the same head re-emitted, tail withdrawn

            clock = ManualClock(0)
            tracker = StabilityTracker(
                StabilityParams(
                    hypotheses_n=rng.choice((2, 3)),
                    retract_clusters_m=rng.choice((2, 3, 5)),
                    hold_ms_t=rng.choice((100, 200, 400)),
                ),
                clock=clock,
            )
            tracker.reset(utterance_id=1)

            with self.subTest(trial=trial, params=tracker.params):
                emitted = run_stream_ticked(tracker, texts, clock, step_ms=120)
                self.assert_stream_conforms(emitted, final_text=sentence)
                self.assert_never_partial_word(emitted)
                # Whatever was published must be a real prefix of the real sentence — not merely
                # of the last hypothesis, which is the check that would pass on garbage.
                published = sentence.encode("utf-8")[: tracker.high_water].decode("utf-8")
                self.assertTrue(sentence.startswith(published))
                self.assertEqual(tracker.finalise(sentence), utf8_len(sentence))

    def test_empty_and_whitespace_only_hypotheses(self):
        t, clock = self.make()
        emitted = run_stream(t, ["", "", "   ", "   ", "hello"], clock)
        self.assert_stream_conforms(emitted)
        for _, stable in emitted:
            self.assertEqual(stable, 0)


if __name__ == "__main__":
    unittest.main()
