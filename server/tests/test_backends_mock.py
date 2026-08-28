# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Tests for :mod:`kvit_asr.backends.mock` and the pseudo-streaming driver in
:mod:`kvit_asr.backends.base`.

Run with::

    C:/Python312/python.exe -m unittest discover -s server/tests -t server

stdlib ``unittest`` only. No numpy, no onnxruntime, no torch — which is the point: the mock is the
piece that makes the transport, the framing codec, the session state machine, the stability
computation, the commit engine and the injector testable on a bare interpreter, so a test suite
that needed a runtime to check it would be checking it in an environment the claim is not about.

─── Why this file exists at all ────────────────────────────────────────────────────────────────

The mock is *infrastructure*, and infrastructure that is trusted without being checked is how a
whole test suite becomes decorative. Four properties carry the weight, and each has a test whose
failure would silently invalidate other people's passing tests:

1. **Determinism under a seed.** Otherwise a failing end-to-end test cannot be reproduced from its
   seed, which is the only handle anyone has on it.
2. **The tail genuinely REVISES, it does not only append.** This is the one that keeps the others
   honest. plan.md §1.4 exists entirely because streaming ASR revises what it already said; a mock
   that only appended would let a broken commit engine pass every end-to-end test in the
   repository. ``test_the_tail_is_revised_not_merely_extended`` asserts a specific word, already
   shown to the client, coming back changed.
3. **Hostile mode really violates the invariant.** docs/protocol.md §8.3's violation handling is
   otherwise untestable end to end, because no correct pipeline can produce its input. If hostile
   mode quietly stopped violating anything, every test that relies on it would keep passing while
   proving nothing — so the violation is asserted against
   :func:`kvit_asr.protocol.check_stability`, an independent implementation of §8.2.
4. **``PseudoStreamingBackend`` emits cumulative hypotheses, not deltas.** docs/protocol.md §8.1
   makes cumulativity the backend's obligation, and a delta produces no error anywhere: the frames
   are well formed, the offsets are in range, the client accepts every message, and the user's
   document fills with fragments.

Determinism is asserted as *self-consistency* (two instances with the same seed agree, and an
utterance is reproducible independently of what ran before it), never as golden strings. The mock's
own docstring is careful that "same seed, same sequence" is a property of CPython's Mersenne
Twister rather than a promise this repository can enforce, and a golden-string test would rot the
day that changed.
"""

import array
import os
import sys
import tempfile
import unittest

_SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from kvit_asr.backends.base import (  # noqa: E402
    BackendCapabilities,
    BackendDecodeError,
    BackendError,
    BackendLoadError,
    BackendStateError,
    PseudoStreamingBackend,
    ResetReason,
    StreamingMode,
    bytes_to_ms,
    ms_to_bytes,
    pcm_samples,
    utf8_len,
)
from kvit_asr.backends.mock import (  # noqa: E402
    DEFAULT_SCRIPT,
    HostileMode,
    MockBackend,
    MockConfig,
    SilenceBehaviour,
    _flip_first_ascii,
    _perturb,
    _word_end_offsets,
)
from kvit_asr.protocol import (  # noqa: E402
    StabilityCheck,
    check_stability,
    is_valid_stable_offset,
)
from kvit_asr.stability import (  # noqa: E402
    ManualClock,
    StabilityParams,
    StabilityTracker,
    StabilityViolation,
    ViolationKind,
)


# ─── Audio ──────────────────────────────────────────────────────────────────────────────────────


def speech(ms: int, amp: int = 5000) -> bytes:
    """Non-silent PCM. The mock's only question of the audio is ``pcm_peak > threshold``."""
    n = ms_to_bytes(ms) // 2
    return array.array("h", [amp if i % 2 else -amp for i in range(n)]).tobytes()


def digital_silence(ms: int) -> bytes:
    return b"\x00" * ms_to_bytes(ms)


def run_utterance(backend, *, chunks=12, chunk_ms=240, finalize=True):
    """Feed ``chunks`` × ``chunk_ms`` of speech, then finalise. Returns the hypotheses in order."""
    out = []
    for _ in range(chunks):
        out.extend(backend.feed(speech(chunk_ms)))
    if finalize:
        final = backend.finalize()
        if final is not None:
            out.append(final)
    return out


def loaded(cfg=None, **overrides) -> MockBackend:
    b = MockBackend(cfg, **overrides)
    b.load()
    b.reset(ResetReason.SESSION_START)
    return b


def texts(hypotheses):
    return [h.text for h in hypotheses]


# ─── Determinism ────────────────────────────────────────────────────────────────────────────────


class TestDeterminism(unittest.TestCase):
    def test_two_instances_with_the_same_seed_agree_exactly(self):
        a = texts(run_utterance(loaded(MockConfig(seed=42))))
        b = texts(run_utterance(loaded(MockConfig(seed=42))))
        self.assertEqual(a, b)
        self.assertGreater(len(a), 3)

    def test_a_different_seed_changes_the_stream(self):
        """Not a strong claim — two seeds could coincide on one utterance — so it is asserted over
        several, where coincidence would mean the seed is not reaching the RNG at all."""
        def three_utterances(seed):
            backend = loaded(MockConfig(seed=seed))
            out = []
            for _ in range(3):
                out.extend(texts(run_utterance(backend)))
                backend.reset(ResetReason.UTTERANCE_BOUNDARY)
            return out

        self.assertNotEqual(three_utterances(1), three_utterances(2))

    def test_an_utterance_is_reproducible_independently_of_what_ran_before_it(self):
        """Per-utterance reseeding, which is what makes a test that exercises one utterance stable
        when a test that exercises three changes.

        The two backends below run utterance 1 *differently* — a different number of emissions,
        therefore a different number of draws from the RNG — and must still agree on utterance 2.
        """
        long_first = loaded(MockConfig(seed=9))
        run_utterance(long_first, chunks=12)
        long_first.reset(ResetReason.UTTERANCE_BOUNDARY)

        short_first = loaded(MockConfig(seed=9))
        run_utterance(short_first, chunks=2)
        short_first.reset(ResetReason.UTTERANCE_BOUNDARY)

        self.assertEqual(
            texts(run_utterance(long_first)), texts(run_utterance(short_first))
        )


# ─── The assertion that keeps the others honest ─────────────────────────────────────────────────


class TestRevision(unittest.TestCase):
    """A mock that only appends is worse than no mock."""

    WORDS = ("alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel")

    def _deterministic(self):
        # One word per emission, one word of tail churn, no deep revisions and no jitter: the
        # emission sequence is then fully predictable and the revision can be asserted word by
        # word rather than "somewhere in the stream".
        return loaded(
            MockConfig(
                seed=3,
                script=(" ".join(self.WORDS),),
                words_per_emission=1,
                words_per_emission_jitter=0,
                revision_depth=1,
                deep_revision_prob=0.0,
            )
        )

    def test_the_tail_is_revised_not_merely_extended(self):
        """The specific, load-bearing property: a word the client has already been shown comes
        back **changed**, not merely followed by more words.

        Emission *k* reveals word *k* in a corrupted form; emission *k+1* restores it and corrupts
        word *k+1*. So for every k the client sees word *k* twice, differently — which is exactly
        the situation plan.md §1.4's committed-prefix invariant exists to survive, and exactly what
        a commit engine that trusted the newest hypothesis would get wrong.
        """
        emitted = texts(run_utterance(self._deterministic(), chunks=20))
        self.assertEqual(len(emitted), len(self.WORDS) + 1)  # 8 emissions + the final

        for k in range(len(self.WORDS) - 1):
            shown = emitted[k].split()
            later = emitted[k + 1].split()
            self.assertEqual(len(shown), k + 1)
            # Word k arrives corrupted...
            self.assertNotEqual(shown[k], self.WORDS[k])
            self.assertEqual(shown[:k], list(self.WORDS[:k]))
            # ...and is repaired by the next hypothesis. That repair is the revision.
            self.assertEqual(later[k], self.WORDS[k])

        # And the whole thing converges: the last emission is clean.
        self.assertEqual(emitted[-1], " ".join(self.WORDS))

    def test_a_hypothesis_is_frequently_not_an_extension_of_its_predecessor(self):
        """The blunt form of the same property, over the realistic default script.

        If this ever passes with zero counted revisions, every end-to-end test in the repository
        that claims to exercise revision handling is exercising nothing.
        """
        emitted = texts(run_utterance(loaded(MockConfig(seed=11))))
        non_extensions = sum(
            1 for a, b in zip(emitted, emitted[1:]) if not b.startswith(a)
        )
        self.assertGreater(non_extensions, 0)

    def test_the_final_differs_from_the_last_partial_when_the_utterance_is_cut_short(self):
        """A real decoder flushes everything it heard, whether or not it had got around to
        revealing it — so a commit engine must handle a ``final`` that is not the last ``partial``
        plus a suffix."""
        emitted = texts(run_utterance(loaded(MockConfig(seed=4)), chunks=3))
        self.assertGreaterEqual(len(emitted), 2)
        self.assertNotEqual(emitted[-1], emitted[-2])
        self.assertGreater(len(emitted[-1]), len(emitted[-2]))

    def test_the_llama_dot_cpp_mis_transcription_is_actually_produced(self):
        """plan.md §4 names ``llama.cpp`` vs ``llama dot cpp`` as the exact error that normalised
        WER hides and that technical-term F1 exists to catch. A mock that never produced it would
        let a harness measuring that metric report a perfect score against a backend that cannot
        spell an identifier."""
        seen = set()
        for seed in range(30):
            for t in texts(run_utterance(loaded(MockConfig(seed=seed)))):
                if "llama dot cpp" in t:
                    seen.add(seed)
        self.assertTrue(seen, "no seed in 0..29 produced the `llama dot cpp` perturbation")

    def test_perturbation_always_changes_a_word_it_can_change(self):
        """``_perturb`` tries transforms in a seeded random order and takes the first that
        actually changes the word, so a revision is produced whenever one is possible rather than
        silently skipped when the randomly-chosen transform happens not to apply."""
        import random

        for word in ("alpha", "config", "llama.cpp", "src/protocol.h", "Qwen3", "café"):
            for seed in range(8):
                self.assertNotEqual(_perturb(word, random.Random(seed)), word, word)


# ─── The mock must survive the stability computation it is used to test ─────────────────────────


class TestStabilitySurvivability(unittest.TestCase):
    """How the mock's default revision behaviour lands against the plan's default stability
    parameters (N=2, M=3, T=200 ms).

    The replay below models the **session loop**, not a bare tracker: ``observe`` on each
    hypothesis, then a clock advance and a ``poll``. That second step is not optional decoration —
    plan.md §1.4 step 5 requires a candidate to have been *unchanged* for T ms, and the mock (like
    a real decoder) grows its candidate on every emission, so without the poll the hold never
    matures and ``stable_bytes`` is 0 for the entire utterance. ``Session.tick`` is what performs
    that poll in the product.
    """

    @staticmethod
    def _replay(seed, **cfg_overrides):
        """Return (published stable_bytes, violation or None) for one utterance."""
        clock = ManualClock()
        tracker = StabilityTracker(StabilityParams(), clock=clock)
        tracker.reset(1)
        published = []
        try:
            for h in run_utterance(loaded(MockConfig(seed=seed, **cfg_overrides))):
                clock.advance(240)              # one emission's worth of audio
                if h.is_final:
                    published.append(tracker.finalise(h.text))
                    continue
                published.append(tracker.observe(h.text))
                clock.advance(240)              # ...then the session loop's tick
                published.append(tracker.poll())
        except StabilityViolation as exc:
            return published, exc
        return published, None

    def test_ordinary_tail_churn_never_violates_the_default_stability_params(self):
        """The mock's ordinary two-word tail churn is what plan.md §1.4 step 3's M-cluster
        retraction exists to absorb, and it absorbs it: no seed produces a violation, and the
        published prefix does advance rather than sitting at zero."""
        for seed in range(25):
            published, violation = self._replay(seed, deep_revision_prob=0.0)
            self.assertIsNone(violation, f"seed {seed}: {violation}")
            self.assertGreater(max(published), 0, f"seed {seed} published nothing")
            self.assertEqual(published, sorted(published), f"seed {seed} went backwards")

    def test_deep_revisions_occasionally_reach_past_the_stable_mark(self):
        """**A measurement, and it contradicts mock.py's docstring — reported rather than papered
        over.**

        mock.py says its defaults are "tuned to be survivable at the plan's defaults (N=2, M=3)".
        Replayed the way the session loop actually drives the tracker (see the class docstring),
        that is *mostly* true but not always: ``deep_revision_prob=0.15`` with
        ``deep_revision_depth=4`` reaches back further than M=3 clusters plus the word-boundary
        retraction can cover, and about 5% of seeds produce ``stable_prefix_mutated``. With
        ``deep_revision_prob=0`` it is 0 seeds out of 200, which identifies the cause exactly.

        This is contained, not catastrophic: ``Session`` converts the violation into a
        session-scoped ``decode_failed`` (docs/protocol.md §11.3, ``fatal:false``) and the
        connection survives. But an end-to-end test that used an unlucky seed and expected a clean
        session would fail for a reason that has nothing to do with the code it was testing, which
        is why it is pinned here and why the seed belongs in such a test's name.

        The assertion is a **rate**, not a seed list: a specific seed would be a golden value
        against CPython's Mersenne Twister, which mock.py explicitly declines to promise.
        """
        violations = [
            seed for seed in range(200) if self._replay(seed)[1] is not None
        ]
        self.assertTrue(
            violations,
            "no seed violated: if the mock's deep revisions have been softened, update this "
            "test and mock.py's docstring together",
        )
        self.assertLessEqual(
            len(violations),
            25,
            f"{len(violations)}/200 seeds violate; the mock is too hostile to be the default "
            f"infrastructure for every other end-to-end test",
        )


# ─── Hostile modes: the deliberate protocol violations ──────────────────────────────────────────


class TestHostileModes(unittest.TestCase):
    """docs/protocol.md §8.3's violation handling has no other input source. If these stopped
    violating anything, the tests that depend on them would pass while proving nothing."""

    def _hostile_stream(self, mode):
        backend = loaded(
            MockConfig(
                seed=5,
                hostile_stability=mode,
                hostile_after_emissions=2,
                words_per_emission=2,
                words_per_emission_jitter=0,
                deep_revision_prob=0.0,
            )
        )
        return [
            (h.text, h.stable_bytes_override, h.hostile)
            for h in run_utterance(backend)
        ]

    def _verdicts(self, stream):
        """Replay the stream through an independent §8.2 implementation."""
        verdicts = []
        prev_text, prev_stable = "", 0
        for text, stable, _hostile in stream:
            if stable is None:
                continue
            verdict, _offset = check_stability(prev_text, prev_stable, text, stable)
            verdicts.append(verdict)
            prev_text, prev_stable = text, stable
        return verdicts

    def test_off_by_default(self):
        for h in run_utterance(loaded(MockConfig(seed=1))):
            self.assertIsNone(h.stable_bytes_override)
            self.assertFalse(h.hostile)

    def test_the_first_emissions_are_well_behaved_so_there_is_something_to_retract(self):
        stream = self._hostile_stream(HostileMode.DECREASE)
        self.assertIsNone(stream[0][1])
        self.assertIsNone(stream[1][1])
        self.assertIsNotNone(stream[2][1])

    def test_decrease_mode_really_decreases(self):
        """docs/protocol.md §8.2 requirement 3, first half; fixture 017. The obvious violation."""
        verdicts = self._verdicts(self._hostile_stream(HostileMode.DECREASE))
        self.assertIn(StabilityCheck.DECREASED, verdicts)

    def test_mutate_mode_really_mutates_the_prefix_without_moving_the_offset(self):
        """§8.2 requirement 3, second half; fixture 018 — **the one with teeth**.

        A receiver that only compares the numbers sees nothing wrong while text it may already
        have typed into the user's document has been retracted. So the assertion is both halves:
        the check must report PREFIX_MUTATED, and ``stable_bytes`` must *not* have decreased —
        otherwise the mode would be exercising the easy violation under a harder name.
        """
        stream = self._hostile_stream(HostileMode.MUTATE)
        verdicts = self._verdicts(stream)
        self.assertIn(StabilityCheck.PREFIX_MUTATED, verdicts)
        self.assertNotIn(StabilityCheck.DECREASED, verdicts)

        marks = [s for _t, s, _h in stream if s is not None]
        self.assertEqual(marks, sorted(marks))

    def test_the_offset_is_a_valid_utf8_grapheme_offset_so_the_violation_is_the_stated_one(self):
        """The point of a hostile mode is to hand the client an offset it must reject for the
        *stated* reason. An offset that was accidentally a code-point index, or that split a UTF-8
        sequence, would be rejected for the wrong one and the test would prove nothing."""
        for mode in (HostileMode.DECREASE, HostileMode.MUTATE):
            for text, stable, _h in self._hostile_stream(mode):
                if stable is None:
                    continue
                self.assertTrue(
                    is_valid_stable_offset(text, stable),
                    f"{mode}: offset {stable} is not a valid stable offset into {text!r}",
                )

    def test_hostile_hypotheses_are_flagged_so_the_server_can_log_the_fact(self):
        for _t, stable, hostile in self._hostile_stream(HostileMode.MUTATE):
            self.assertEqual(hostile, stable is not None)

    def test_capabilities_announce_the_hostile_mode_in_their_notes(self):
        caps = MockBackend(MockConfig(hostile_stability=HostileMode.MUTATE)).capabilities
        self.assertIn("HOSTILE", caps.notes)
        self.assertIn("mutate", caps.notes)

    def test_the_override_is_the_only_route_to_a_violation(self):
        """The other side of the same coin, and the reason ``stable_bytes_override`` had to be
        invented at all: a correct server **cannot** be talked into putting an inadmissible
        ``stable_bytes`` on the wire.

        Feed the hostile mode's *text* stream — ignoring its override — to the real stability
        tracker. It publishes a prefix from the well-behaved opening emissions, then MUTATE's case
        flip retracts bytes below that mark, and the tracker **raises** rather than clamping.
        §8.3 forbids clamping precisely because clamping would hide the defect ICER exists to
        measure, and ``Session`` turns the raise into a session-scoped ``decode_failed``.

        So no correct pipeline can generate the input the *Client's* §8.3 handling must survive.
        That is the entire justification for a test hook that forces one.
        """
        clock = ManualClock()
        tracker = StabilityTracker(StabilityParams(), clock=clock)
        tracker.reset(1)
        published = []
        with self.assertRaises(StabilityViolation) as ctx:
            for text, _stable, _hostile in self._hostile_stream(HostileMode.MUTATE):
                clock.advance(240)
                published.append(tracker.observe(text))
                clock.advance(240)
                published.append(tracker.poll())

        self.assertEqual(ctx.exception.kind, ViolationKind.STABLE_PREFIX_MUTATED)
        self.assertEqual(ctx.exception.diverge_offset, 0)   # the flipped first byte
        # The violation is a genuine retraction: something had already been published under it.
        self.assertGreater(max(published), 0)
        self.assertGreater(ctx.exception.previous_stable_bytes, 0)

    def test_the_well_behaved_default_backend_does_let_the_tracker_publish(self):
        """The control for the test above: all-zero ``stable_bytes`` would also be produced by a
        tracker that had simply stopped working."""
        clock = ManualClock()
        tracker = StabilityTracker(StabilityParams(), clock=clock)
        tracker.reset(1)
        published = []
        for h in run_utterance(loaded(MockConfig(seed=5, deep_revision_prob=0.0))):
            clock.advance(240)
            published.append(tracker.observe(h.text))
            clock.advance(240)
            published.append(tracker.poll())
        self.assertGreater(max(published), 0)


class TestByteOffsetHelpers(unittest.TestCase):
    def test_word_end_offsets_count_utf8_bytes_not_code_points(self):
        # "café" is 4 code points and 5 UTF-8 bytes. An offsets helper that returned 4 would look
        # right in every ASCII test and be wrong on a user's first accented word.
        self.assertEqual(_word_end_offsets("café bill"), [5, 10])
        self.assertEqual(len("café"), 4)
        self.assertEqual(utf8_len("café"), 5)

    def test_word_end_offsets_exclude_trailing_whitespace(self):
        # docs/protocol.md §8.2: holding the space back lets a following punctuation revision land
        # without rewriting across committed text.
        self.assertEqual(_word_end_offsets("ab cd  "), [2, 5])
        self.assertEqual(_word_end_offsets("   "), [])
        self.assertEqual(_word_end_offsets(""), [])

    def test_flip_first_ascii_preserves_the_byte_length(self):
        """Length preservation is the whole trick of MUTATE: the offset stays numerically
        identical while the bytes below it differ."""
        for text in ("can you", "café bill", "👍🏽 ship it", "12 34"):
            self.assertEqual(utf8_len(_flip_first_ascii(text)), utf8_len(text))
            if any(c.isascii() and not c.isspace() for c in text):
                self.assertNotEqual(_flip_first_ascii(text), text)


# ─── Silence: the hallucination fixture ─────────────────────────────────────────────────────────


class TestSilence(unittest.TestCase):
    def test_quiet_is_the_default_and_emits_nothing_over_silence(self):
        """The control. A correctly gated pipeline should look like this."""
        backend = loaded(MockConfig(seed=1))
        self.assertEqual(backend.feed(digital_silence(3000)), [])
        self.assertIsNone(backend.finalize())

    def test_hallucinate_mode_invents_text_over_silence(self):
        """plan.md §1.6 records Whisper emitting "Thank you." / "Bye" over trailing silence with
        timestamps past the end of the audio. Without a backend that can be made to do this on
        demand, the VAD gating that prevents it ships untested."""
        backend = loaded(
            MockConfig(seed=1, silence_behaviour=SilenceBehaviour.HALLUCINATE)
        )
        emitted = []
        for _ in range(6):
            emitted.extend(texts(backend.feed(digital_silence(500))))
        self.assertTrue(emitted)
        phrases = MockConfig().hallucinations
        for text in emitted:
            self.assertTrue(
                any(p in text for p in phrases), f"unexpected hallucination {text!r}"
            )

    def test_nothing_is_hallucinated_before_hallucinate_after_ms(self):
        """``hallucinate_after_ms`` is deliberately near plan.md §1.6's 0.8 s endpoint, so a
        correctly gated pipeline never reaches it and an ungated one reaches it immediately."""
        backend = loaded(
            MockConfig(
                seed=1,
                silence_behaviour=SilenceBehaviour.HALLUCINATE,
                hallucinate_after_ms=800,
            )
        )
        self.assertEqual(backend.feed(digital_silence(700)), [])
        self.assertTrue(backend.feed(digital_silence(200)))

    def test_speech_resets_the_silence_run(self):
        backend = loaded(
            MockConfig(seed=1, silence_behaviour=SilenceBehaviour.HALLUCINATE)
        )
        backend.feed(digital_silence(700))
        backend.feed(speech(240))
        self.assertEqual(backend.feed(digital_silence(700)), [])

    def test_the_silence_threshold_is_a_peak_not_an_average(self):
        cfg = MockConfig(seed=1, silence_peak_threshold=500)
        quiet = loaded(cfg)
        self.assertEqual(quiet.feed(speech(1000, amp=400)), [])   # under the threshold
        loud = loaded(MockConfig(seed=1, silence_peak_threshold=500))
        self.assertTrue(loud.feed(speech(1000, amp=600)))


# ─── Context / vocabulary biasing ───────────────────────────────────────────────────────────────


class TestContext(unittest.TestCase):
    SCRIPT = ("llama.cpp llama.cpp llama.cpp llama.cpp",)

    def _cfg(self, supports):
        return MockConfig(
            seed=2,
            script=self.SCRIPT,
            supports_context=supports,
            words_per_emission=1,
            words_per_emission_jitter=0,
            revision_depth=1,
            deep_revision_prob=0.0,
        )

    def test_a_backend_without_context_support_says_so_rather_than_pretending(self):
        """docs/protocol.md §6.1 requires ``context_supported:false`` rather than a silent drop,
        because plan.md §4 scores context-enabled and context-free runs in separate columns."""
        backend = loaded(self._cfg(False))
        self.assertFalse(backend.set_context(["llama.cpp"]))
        self.assertEqual(backend.context, ("llama.cpp",))
        self.assertFalse(backend.capabilities.supports_context)

    def test_without_context_the_hinted_term_is_mis_transcribed_at_some_point(self):
        backend = loaded(self._cfg(False))
        backend.set_context(["llama.cpp"])
        emitted = texts(run_utterance(backend, chunks=10))
        self.assertTrue(
            any("llama.cpp llama.cpp llama.cpp llama.cpp" != t for t in emitted[:-1])
        )
        self.assertTrue(any("llama.cpp" not in t.split()[-1] for t in emitted[:-1]))

    def test_with_context_the_hinted_term_is_never_perturbed(self):
        """The mock's model of biasing is deliberately optimistic — a real biased decoder raises a
        probability, it does not guarantee a term. What matters for the harness is that the two
        modes differ **measurably and in the expected direction**, so a bug in the
        ``start.context`` plumbing cannot masquerade as "biasing did not help much"."""
        backend = loaded(self._cfg(True))
        self.assertTrue(backend.set_context(["llama.cpp"]))
        for text in texts(run_utterance(backend, chunks=10)):
            for word in text.split():
                self.assertEqual(word, "llama.cpp")

    def test_clearing_the_context_removes_the_protection(self):
        backend = loaded(self._cfg(True))
        backend.set_context(["llama.cpp"])
        backend.set_context([])
        emitted = texts(run_utterance(backend, chunks=10))
        self.assertTrue(any(w != "llama.cpp" for t in emitted for w in t.split()))


# ─── Scripts ────────────────────────────────────────────────────────────────────────────────────


class TestScripts(unittest.TestCase):
    def test_the_default_script_carries_the_hazards_it_was_chosen_for(self):
        joined = " ".join(DEFAULT_SCRIPT)
        self.assertIn("llama.cpp", joined)          # identifier with internal punctuation
        self.assertIn("src/protocol.h", joined)     # path
        self.assertIn("café", joined)               # multi-byte
        self.assertIn("\u200d", joined)             # ZWJ emoji sequence (fixture 013's material)

    def test_a_script_file_is_read_as_utf8_with_comments_and_blanks_ignored(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "script.txt")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("# a comment\n\nfirst café line\n   second line   \n")
            backend = loaded(MockConfig(script_path=path))
            self.assertEqual(backend.script, ("first café line", "second line"))

    def test_a_non_utf8_script_is_refused_rather_than_leniently_decoded(self):
        """A lenient decode would inject replacement characters into text destined for a keystroke
        injector, which docs/protocol.md §4.1 forbids on the wire for the same reason."""
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "latin1.txt")
            with open(path, "wb") as fh:
                fh.write(b"caf\xe9 bill\n")
            with self.assertRaises(BackendLoadError) as ctx:
                MockBackend(MockConfig(script_path=path)).load()
            self.assertIn("UTF-8", str(ctx.exception))

    def test_a_missing_script_file_is_a_load_error_naming_the_path(self):
        with self.assertRaises(BackendLoadError) as ctx:
            MockBackend(MockConfig(script_path="no-such-script.txt")).load()
        self.assertIn("no-such-script.txt", str(ctx.exception))

    def test_an_empty_script_is_refused(self):
        with self.assertRaises(BackendLoadError):
            MockBackend(MockConfig(script=("", "   ", "# only a comment"))).load()

    def test_loop_script_wraps_around(self):
        backend = loaded(MockConfig(seed=1, script=("one two three", "four five six")))
        first = texts(run_utterance(backend))[-1]
        backend.reset(ResetReason.UTTERANCE_BOUNDARY)
        second = texts(run_utterance(backend))[-1]
        backend.reset(ResetReason.UTTERANCE_BOUNDARY)
        third = texts(run_utterance(backend))[-1]
        self.assertEqual(first, "one two three")
        self.assertEqual(second, "four five six")
        self.assertEqual(third, "one two three")

    def test_loop_script_false_runs_out_of_text(self):
        """"A useful way to test the server's ``audio_underrun`` path" — the server turns an
        utterance that decodes to nothing into ``audio_underrun`` rather than an empty ``final``."""
        backend = loaded(MockConfig(seed=1, script=("only line",), loop_script=False))
        run_utterance(backend)
        backend.reset(ResetReason.UTTERANCE_BOUNDARY)
        emitted = run_utterance(backend)
        self.assertEqual([h.text for h in emitted], [""])
        self.assertTrue(emitted[0].is_final)


# ─── Lifecycle, faults and declarations ─────────────────────────────────────────────────────────


class TestLifecycle(unittest.TestCase):
    def test_feed_before_load_is_a_state_error(self):
        with self.assertRaises(BackendStateError):
            MockBackend().feed(speech(100))

    def test_finalize_before_load_is_a_state_error(self):
        with self.assertRaises(BackendStateError):
            MockBackend().finalize()

    def test_an_odd_length_chunk_is_rejected_as_bad_message(self):
        """It should never reach a backend — which is exactly why it is checked here. If it does,
        something upstream is slicing buffers wrongly, and a loud failure is far cheaper than
        half-rate garbled audio."""
        backend = loaded()
        with self.assertRaises(BackendError) as ctx:
            backend.feed(b"\x01\x02\x03")
        self.assertEqual(ctx.exception.wire_code, "bad_message")

    def test_an_empty_chunk_is_a_no_op(self):
        # docs/protocol.md §6.3 makes a zero-length audio payload legal (fixture 003).
        self.assertEqual(loaded().feed(b""), [])

    def test_injected_decode_failure_is_non_fatal_and_maps_to_decode_failed(self):
        """docs/protocol.md §11.3: the child is not at fault as a *process*; the session is
        aborted and the connection survives."""
        backend = loaded(MockConfig(seed=1, decode_failure_after_ms=500))
        with self.assertRaises(BackendDecodeError) as ctx:
            for _ in range(10):
                backend.feed(speech(240))
        self.assertEqual(ctx.exception.wire_code, "decode_failed")
        self.assertFalse(ctx.exception.fatal)

    def test_unknown_override_names_are_refused_rather_than_ignored(self):
        with self.assertRaises(TypeError):
            MockBackend(seeed=1)

    def test_invalid_config_values_are_refused(self):
        for kwargs in (
            {"emit_every_ms": 0},
            {"words_per_emission": 0},
            {"words_per_emission_jitter": -1},
            {"deep_revision_prob": 1.5},
        ):
            with self.assertRaises(ValueError, msg=repr(kwargs)):
                MockConfig(**kwargs)

    def test_hallucinate_mode_needs_at_least_one_phrase(self):
        with self.assertRaises(ValueError):
            MockConfig(
                silence_behaviour=SilenceBehaviour.HALLUCINATE, hallucinations=()
            )

    def test_warm_up_does_not_advance_the_script(self):
        """A real backend has no positional counter; this is the price of a mock whose output is
        positional. Without the save/restore, a warmed server would start the user's first real
        session on utterance 2."""
        backend = MockBackend(MockConfig(seed=1, script=("first line", "second line")))
        backend.load()
        backend.warm_up(audio_ms=400)
        backend.reset(ResetReason.SESSION_START)
        self.assertEqual(texts(run_utterance(backend))[-1], "first line")

    def test_warm_up_measures_something_and_leaves_no_text_behind(self):
        backend = MockBackend(MockConfig(seed=1))
        backend.load()
        self.assertIsNone(backend.warm_ms)
        backend.warm_up(audio_ms=300)
        self.assertIsNotNone(backend.warm_ms)
        self.assertGreaterEqual(backend.warm_ms, 0)
        self.assertIsNotNone(backend.load_ms)

    def test_load_is_idempotent(self):
        backend = MockBackend(MockConfig(seed=1))
        self.assertFalse(backend.is_loaded)
        backend.load()
        self.assertTrue(backend.is_loaded)
        backend.load()
        backend.unload()
        self.assertFalse(backend.is_loaded)

    def test_latency_injection_goes_through_sleep_fn_so_tests_need_not_wait(self):
        requested = []
        backend = loaded(
            MockConfig(seed=1, latency_ms=250, sleep_fn=requested.append)
        )
        backend.feed(speech(240))
        self.assertEqual(requested, [0.25])

    def test_discontinuity_does_not_reset_this_backend(self):
        """``requires_contiguous_audio`` is False on purpose: the mock's "decoder state" is an
        index into a word list, so a gap costs it nothing. Declaring True would make
        ``on_discontinuity`` reset the utterance and mask whether the *server* handles
        discontinuity correctly."""
        backend = loaded(MockConfig(seed=1))
        before = texts(run_utterance(backend, chunks=4, finalize=False))
        backend.on_discontinuity(gap_ms=120)
        after = texts(run_utterance(backend, chunks=1, finalize=False))
        self.assertTrue(after)
        self.assertTrue(after[0].startswith(before[0].split()[0]))


class TestDeclaration(unittest.TestCase):
    def test_capabilities_are_answerable_before_load(self):
        """So the server can log what it is about to load, and a manifest can be validated without
        paying the load cost."""
        caps = MockBackend(MockConfig(model_id="mock-x")).capabilities
        self.assertIsInstance(caps, BackendCapabilities)
        self.assertEqual(caps.model_id, "mock-x")

    def test_the_mock_can_impersonate_each_declared_streaming_mode(self):
        """plan.md §3 Phase 0A's taxonomy is the reason latency numbers stay interpretable, so the
        server's ``ready.mode`` plumbing needs to be exercisable for all three without three real
        runtimes."""
        for mode in StreamingMode:
            caps = MockBackend(MockConfig(mode=mode)).capabilities
            self.assertEqual(caps.mode, mode)
            self.assertEqual(caps.ready_fields()["mode"], mode.value)

    def test_ready_fields_use_the_wire_spellings(self):
        caps = MockBackend(MockConfig(chunk_ms=160)).capabilities
        fields = caps.ready_fields()
        self.assertEqual(
            set(fields), {"model", "mode", "context_supported", "chunk_ms"}
        )
        self.assertNotIn("warm_ms", fields)   # a process-start measurement the backend cannot make

    def test_describe_is_flat_and_json_friendly(self):
        backend = loaded(MockConfig(seed=1))
        described = backend.describe()
        import json

        json.dumps(described)   # must not raise: it goes into bench/results/*.json
        self.assertEqual(described["mode"], StreamingMode.NATIVE_STREAMING.value)


# ─── PseudoStreamingBackend: cumulative, not delta ──────────────────────────────────────────────
#
# The fake batch model below reads its answer out of the audio itself: each 200 ms block is filled
# with a constant amplitude, and `_transcribe_batch` turns each block into the token `w<amplitude>`.
# That makes the transcript of any buffer exactly predictable *and* dependent only on the audio the
# batch model can actually see — which is what lets the window-sliding and prefix-freezing be
# asserted rather than hoped about.

BLOCK_MS = 200
BLOCK_SAMPLES = 16000 * BLOCK_MS // 1000


def block(value: int) -> bytes:
    return array.array("h", [value] * BLOCK_SAMPLES).tobytes()


class _AmplitudeReader(PseudoStreamingBackend):
    def __init__(self, **kw):
        self.batches = 0
        super().__init__(**kw)

    @property
    def capabilities(self):
        return BackendCapabilities(
            model_id="amplitude-reader", mode=StreamingMode.PSEUDO_STREAMING
        )

    def _load(self, progress):
        pass

    def _transcribe_batch(self, pcm: bytes) -> str:
        self.batches += 1
        samples = pcm_samples(pcm)
        tokens = []
        for start in range(0, len(samples) - BLOCK_SAMPLES + 1, BLOCK_SAMPLES):
            tokens.append("w%d" % samples[start])
        return " ".join(tokens)


class _MislabelledWrapper(_AmplitudeReader):
    @property
    def capabilities(self):
        return BackendCapabilities(
            model_id="liar", mode=StreamingMode.NATIVE_STREAMING
        )


class TestPseudoStreaming(unittest.TestCase):
    def _driver(self):
        b = _AmplitudeReader(window_ms=1000, hop_ms=BLOCK_MS)
        b.load()
        b.reset(ResetReason.SESSION_START)
        return b

    def test_hypotheses_are_cumulative_not_deltas(self):
        """docs/protocol.md §8.1. A delta produces no error anywhere: the frames are well formed,
        the offsets are in range, the client accepts every message, and the user's document fills
        with fragments. So this is asserted against the exact expected text, not against a shape.
        """
        backend = self._driver()
        emitted = []
        for value in range(1, 13):
            emitted.extend(texts(backend.feed(block(value))))

        self.assertEqual(len(emitted), 12)
        for i, text in enumerate(emitted, start=1):
            self.assertEqual(text, " ".join("w%d" % v for v in range(1, i + 1)))

        # The window is 5 blocks; 12 blocks were fed, so it slid — and the hypothesis still spans
        # everything, which is only possible because the scrolled-off text was frozen.
        self.assertEqual(backend.frozen_prefix, " ".join("w%d" % v for v in range(1, 8)))

    def test_each_hypothesis_extends_its_predecessor_word_for_word(self):
        backend = self._driver()
        emitted = []
        for value in range(1, 13):
            emitted.extend(texts(backend.feed(block(value))))
        for a, b in zip(emitted, emitted[1:]):
            self.assertTrue(b.startswith(a), f"{b!r} does not extend {a!r}")

    def test_the_window_really_slides_so_the_batch_model_sees_a_bounded_buffer(self):
        """Otherwise the test above would be satisfied by a wrapper that simply re-transcribes an
        unbounded buffer, which is not what a windowed driver does and would hide the freeze."""
        backend = self._driver()
        for value in range(1, 13):
            backend.feed(block(value))
        self.assertEqual(backend.window_ms, 1000)
        self.assertEqual(backend.hop_ms, BLOCK_MS)
        self.assertEqual(backend.batches, 12)

    def test_finalize_returns_the_complete_text_and_none_when_no_audio_arrived(self):
        backend = self._driver()
        self.assertIsNone(backend.finalize())
        for value in range(1, 4):
            backend.feed(block(value))
        final = backend.finalize()
        self.assertIsNotNone(final)
        self.assertTrue(final.is_final)
        self.assertEqual(final.text, "w1 w2 w3")

    def test_repeated_words_can_mis_stitch_which_is_the_documented_cost(self):
        """base.py is explicit that word-overlap stitching "can be wrong" and that repeated words
        make the overlap ambiguous. Asserted so the limitation stays known: 12 identical blocks
        produce a hypothesis *shorter* than the audio, because the stitcher cannot tell which
        repetition scrolled off.

        This is a limitation of pseudo-streaming, not a bug to fix here — plan.md §3 Phase 0A adds
        the streaming-mode column precisely so numbers from this mode are read separately.
        """
        backend = self._driver()
        last = ""
        for _ in range(12):
            for h in backend.feed(block(7)):
                last = h.text
        self.assertEqual(last.split(), ["w7"] * 5)      # the window, and nothing frozen
        self.assertEqual(backend.frozen_prefix, "")

    def test_a_subclass_that_mislabels_its_mode_is_refused_at_load(self):
        """Refusing rather than correcting: a wrapper that silently relabelled itself would put
        uncomparable latency numbers in the same column as the native-streaming rows."""
        with self.assertRaises(BackendLoadError) as ctx:
            _MislabelledWrapper(window_ms=1000, hop_ms=BLOCK_MS).load()
        self.assertIn("pseudo-streaming", str(ctx.exception))

    def test_construction_rejects_an_impossible_window(self):
        with self.assertRaises(ValueError):
            _AmplitudeReader(window_ms=100, hop_ms=200)
        with self.assertRaises(ValueError):
            _AmplitudeReader(window_ms=1000, hop_ms=0)

    def test_reset_drops_the_frozen_prefix(self):
        backend = self._driver()
        for value in range(1, 13):
            backend.feed(block(value))
        self.assertTrue(backend.frozen_prefix)
        backend.reset(ResetReason.UTTERANCE_BOUNDARY)
        self.assertEqual(backend.frozen_prefix, "")
        self.assertIsNone(backend.finalize())


class TestPcmArithmetic(unittest.TestCase):
    def test_ms_and_byte_conversions_agree(self):
        self.assertEqual(ms_to_bytes(1000), 32000)
        self.assertEqual(bytes_to_ms(32000), 1000)
        self.assertEqual(bytes_to_ms(31999), 999)   # rounds down; never invents a sample


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
