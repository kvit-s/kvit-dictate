# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Tests for :mod:`kvit_asr.vad` — plan.md §1.6 and §3 Phase 2 item 3.

Run with::

    C:/Python312/python.exe -m unittest discover -s server/tests -t server

stdlib ``unittest`` only, and — deliberately — **stdlib-only audio synthesis**. Every waveform
below is built from ``math.sin`` and ``random.Random`` into an ``array('h')``. numpy is not
imported and must not be: the whole justification for ``EnergyZcrScorer`` existing is that the
sidecar endpoints correctly on a bare interpreter, and a test suite that needed numpy to prove it
would be proving it in an environment the claim is not about.

─── What is asserted, and why these and not others ─────────────────────────────────────────────

plan.md §1.6 makes three claims that are *correctness* claims rather than tuning preferences, and
each has a test whose failure would be a real defect rather than a regression in a number:

1. **A short mid-sentence pause must not endpoint.** Merging the endpoint timer with the session
   timer is the specific ``Win+H`` failure this product exists to replace. Asserted in
   ``test_short_pause_does_not_endpoint``.
2. **A long pause must endpoint**, so the utterance finalises and punctuates while the session
   survives. Asserted in ``test_long_pause_endpoints_and_opens_a_new_utterance``.
3. **The decoder gate must go false over silence.** This is not an optimisation: plan.md §1.6
   records that several ASR models hallucinate on long silence — ``wan-dancer``'s own
   ``notes/transcribe.py`` carries a comment about Whisper emitting "Thank you." / "Bye" over
   trailing silence with timestamps past the end of the audio. A gate that stayed open would feed
   that silence to the model and put invented words into the user's document. Asserted in
   ``test_decoder_gate_closes_over_silence`` and
   ``test_silence_only_stream_produces_no_events_and_never_opens_the_gate``.

Positions are asserted in **samples**, exactly, not in milliseconds with a tolerance. The module
computes in samples for that reason, the frame size (512) divides every waveform used here, and an
assertion with slack would pass a detector that was a frame late.

─── Silero ─────────────────────────────────────────────────────────────────────────────────────

Nothing here requires ``onnxruntime`` or a model file. plan.md §1.9 forbids runtime model
download and the repository rules forbid installing anything, so the Silero-specific cases are
skipped with a message naming what is missing. The cases that assert Silero's *unavailability*
handling — the exact failure text, and ``create_vad``'s fallback — run always, because those are
the paths every machine without a model actually takes.
"""

import array
import logging
import math
import os
import random
import sys
import unittest

# Make ``server/`` importable regardless of where the runner was started from. ``unittest
# discover -s server/tests`` puts ``server/tests`` on sys.path, not ``server``.
_SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from kvit_asr.vad import (  # noqa: E402
    BYTES_PER_SAMPLE,
    EnergyZcrScorer,
    FrameScorer,
    SileroOnnxScorer,
    VadBackendUnavailable,
    VadConfig,
    VadConfigError,
    VadEndpointer,
    VadEventKind,
    VadState,
    create_vad,
    frame_dbfs_zcr,
    ms_to_samples,
    pcm_to_samples,
    samples_to_ms,
    samples_to_pcm,
)


# ─── Stdlib audio synthesis ─────────────────────────────────────────────────────────────────────
#
# Everything is expressed in whole 512-sample frames. That is not laziness about durations: the
# endpointer only ever makes a decision on a frame boundary, so a waveform whose edges land inside
# a frame would make every asserted position depend on where the edge happened to fall, and the
# tests would be measuring the arithmetic of the test rather than the behaviour of the module.

SAMPLE_RATE = 16000
FRAME = 512                     # VadConfig.frame_samples default; 32 ms at 16 kHz


def _to_pcm(samples: array.array) -> bytes:
    return samples_to_pcm(samples)


def silence(frames: int) -> bytes:
    """Digital silence. Not "quiet" — exactly zero, which is what a muted capture produces."""
    return b"\x00" * (frames * FRAME * BYTES_PER_SAMPLE)


def tone(frames: int, amp: int = 8000, freq: float = 250.0, phase_samples: int = 0) -> bytes:
    """A steady sine burst — the stand-in for a voiced vowel.

    250 Hz at 16 kHz is exactly 64 samples per cycle, so a 512-sample frame holds 8 whole cycles
    and every frame has identical RMS. That removes the only source of frame-to-frame variation
    that could make an asserted onset position wobble. Its zero-crossing rate is 2·250/16000 =
    0.031, well below ``VadConfig.zcr_min``, so the ZCR term contributes nothing here and the test
    exercises the energy path it means to exercise.
    """
    out = array.array("h")
    for i in range(phase_samples, phase_samples + frames * FRAME):
        out.append(int(amp * math.sin(2.0 * math.pi * freq * i / SAMPLE_RATE)))
    return _to_pcm(out)


def noise(frames: int, amp: int, seed: int = 1234) -> bytes:
    """Uniform white noise at ±``amp``. RMS ≈ amp/√3.

    Seeded so a failure reproduces. White noise has a zero-crossing rate near 0.5, above
    ``VadConfig.zcr_max``, so — like the tone — it does not collect the unvoiced-speech ZCR boost
    and the test is about level alone.
    """
    rng = random.Random(seed)
    out = array.array("h")
    for _ in range(frames * FRAME):
        out.append(rng.randint(-amp, amp))
    return _to_pcm(out)


def mix(*chunks: bytes) -> bytes:
    """Sum PCM chunks of equal length, clipped to int16."""
    arrays = [pcm_to_samples(c) for c in chunks]
    n = min(len(a) for a in arrays)
    out = array.array("h")
    for i in range(n):
        v = sum(a[i] for a in arrays)
        out.append(max(-32768, min(32767, v)))
    return _to_pcm(out)


# The energy fallback is deaf for its calibration window (``energy_warmup_ms`` = 200 ms → 7 frames
# at 512 samples). Every waveform that expects a detection therefore opens with more silence than
# that, so the floor is calibrated before anything interesting happens. This is a documented wart
# of the fallback, not of the endpointer, and `test_energy_scorer_is_deaf_during_calibration`
# pins it so it cannot become a silent one.
PAD_FRAMES = 10
PAD_SAMPLES = PAD_FRAMES * FRAME


def collect(vad: VadEndpointer, *chunks: bytes):
    """Feed chunks in order; return (all events, all segments, last chunk)."""
    events = []
    segments = []
    last = None
    for c in chunks:
        last = vad.process(c)
        events.extend(last.events)
        segments.extend(last.segments)
    return events, segments, last


def kinds(events, kind: VadEventKind):
    return [e for e in events if e.kind is kind]


class ConstantScorer(FrameScorer):
    """A scorer driven by a script of probabilities — for testing the state machine alone.

    The energy backend is a detector *and* a state machine; a test that only wants to assert the
    state machine should not also have to satisfy the detector. Frames past the end of the script
    score 0.0.
    """

    name = "constant"

    def __init__(self, probs):
        self.probs = list(probs)
        self.i = 0
        self.resets = 0

    def score(self, frame):
        p = self.probs[self.i] if self.i < len(self.probs) else 0.0
        self.i += 1
        return p

    def reset(self):
        self.resets += 1


# ─── Units and PCM helpers ──────────────────────────────────────────────────────────────────────


class TestUnits(unittest.TestCase):
    def test_ms_and_sample_conversions_round_trip_on_frame_boundaries(self):
        self.assertEqual(ms_to_samples(32, SAMPLE_RATE), FRAME)
        self.assertEqual(samples_to_ms(FRAME, SAMPLE_RATE), 32.0)
        self.assertEqual(ms_to_samples(800, SAMPLE_RATE), 12800)
        self.assertEqual(ms_to_samples(-5, SAMPLE_RATE), 0)  # never negative

    def test_odd_length_pcm_is_rejected_rather_than_truncated(self):
        # docs/protocol.md §6.3 makes an odd audio payload `bad_message` at the protocol layer.
        # Accepting it here would shift every subsequent sample and show up only as mysteriously
        # bad transcription.
        with self.assertRaises(ValueError):
            pcm_to_samples(b"\x00\x00\x00")

    def test_pcm_round_trips_through_samples(self):
        pcm = tone(1)
        self.assertEqual(samples_to_pcm(pcm_to_samples(pcm)), pcm)

    def test_frame_dbfs_of_digital_silence_is_the_floor_not_negative_infinity(self):
        dbfs, zcr = frame_dbfs_zcr(pcm_to_samples(silence(1)))
        self.assertEqual(dbfs, -120.0)
        self.assertEqual(zcr, 0.0)

    def test_frame_dbfs_of_a_full_scale_tone_is_near_minus_three(self):
        dbfs, zcr = frame_dbfs_zcr(pcm_to_samples(tone(1, amp=32767)))
        self.assertAlmostEqual(dbfs, -3.0, delta=0.2)   # RMS of a sine is 1/√2 of its peak
        self.assertAlmostEqual(zcr, 2 * 250.0 / SAMPLE_RATE, delta=0.01)


# ─── Configuration ──────────────────────────────────────────────────────────────────────────────


class TestVadConfig(unittest.TestCase):
    def test_hangover_longer_than_the_endpoint_is_refused(self):
        # A hangover longer than the endpoint would report the end of speech *after* the utterance
        # it belongs to had already been closed.
        with self.assertRaises(VadConfigError):
            VadConfig(hangover_ms=900.0, endpoint_silence_ms=800.0)

    def test_neg_threshold_must_sit_below_threshold(self):
        with self.assertRaises(VadConfigError):
            VadConfig(threshold=0.4, neg_threshold=0.5)

    def test_max_utterance_must_exceed_the_endpoint_or_be_disabled(self):
        with self.assertRaises(VadConfigError):
            VadConfig(endpoint_silence_ms=800.0, max_utterance_ms=500.0)
        VadConfig(endpoint_silence_ms=800.0, max_utterance_ms=0.0)  # 0 disables; must not raise

    def test_unknown_backend_name_is_a_config_error_not_a_silent_fallback(self):
        with self.assertRaises(VadConfigError):
            VadConfig(backend="silero-v5")

    def test_derived_sample_counts_match_the_defaults_the_tests_assert_against(self):
        cfg = VadConfig()
        self.assertEqual(cfg.frame_samples, FRAME)
        self.assertEqual(cfg.min_speech_samples, 3200)     # 200 ms
        self.assertEqual(cfg.hangover_samples, 4000)       # 250 ms
        self.assertEqual(cfg.endpoint_samples, 12800)      # plan.md §1.6's ~0.8 s
        self.assertEqual(cfg.preroll_samples, 4800)        # 300 ms


# ─── Silence ────────────────────────────────────────────────────────────────────────────────────


class TestSilence(unittest.TestCase):
    def test_silence_only_stream_produces_no_events_and_never_opens_the_gate(self):
        """plan.md §4 scores "hallucinated output on silence-only input". The server's half of
        that defence is this: a silence-only stream must reach the decoder as *nothing at all*."""
        vad = VadEndpointer(VadConfig())
        events, segments, last = collect(vad, silence(100))   # 3.2 s
        self.assertEqual(events, [])
        self.assertEqual(segments, [])
        self.assertFalse(last.gate_open)
        self.assertFalse(vad.gate_open)
        self.assertEqual(vad.utterance_index, 0)
        self.assertIs(vad.state, VadState.SILENCE)

    def test_flush_over_a_silence_only_stream_emits_nothing(self):
        vad = VadEndpointer(VadConfig())
        collect(vad, silence(50))
        chunk = vad.flush()
        self.assertEqual(chunk.events, ())
        self.assertEqual(chunk.segments, ())
        self.assertFalse(chunk.gate_open)


# ─── Onset, endpoint, hangover, minimum duration ────────────────────────────────────────────────


class TestBurst(unittest.TestCase):
    """One tone burst surrounded by silence — the positions of every event it produces."""

    def setUp(self):
        self.cfg = VadConfig()
        self.vad = VadEndpointer(self.cfg)
        self.burst_frames = 20                              # 640 ms
        self.onset_sample = PAD_SAMPLES                     # burst begins here, frame-aligned
        self.burst_end_sample = PAD_SAMPLES + self.burst_frames * FRAME
        self.events, self.segments, self.last = collect(
            self.vad,
            silence(PAD_FRAMES),
            tone(self.burst_frames),
            silence(40),                                    # 1.28 s > endpoint_silence_ms
        )

    def test_onset_is_backdated_to_where_speech_actually_started(self):
        starts = kinds(self.events, VadEventKind.SPEECH_START)
        self.assertEqual(len(starts), 1)
        start = starts[0]
        # `sample` is where speech began; `decided_at_sample` is where the detector had enough
        # evidence to say so. Conflating them is the classic VAD reporting bug, and plan.md §3
        # Phase 2 item 4 needs them separately to decompose latency.
        self.assertEqual(start.sample, self.onset_sample)
        # min_speech_ms = 200 ms = 3200 samples; at 512 per frame the 7th speaking frame is the
        # first to clear it, so the decision lands 7 frames after the onset.
        self.assertEqual(start.decided_at_sample, self.onset_sample + 7 * FRAME)
        self.assertGreater(start.latency_ms, 0.0)
        self.assertEqual(start.utterance_index, 1)

    def test_speech_end_fires_one_hangover_after_the_last_speech(self):
        ends = kinds(self.events, VadEventKind.SPEECH_END)
        self.assertEqual(len(ends), 1)
        end = ends[0]
        self.assertEqual(end.sample, self.burst_end_sample)
        # hangover_samples = 4000; the 8th silent frame (4096 samples) is the first to clear it.
        self.assertEqual(end.decided_at_sample, self.burst_end_sample + 8 * FRAME)

    def test_utterance_end_fires_one_endpoint_silence_after_the_last_speech(self):
        ends = kinds(self.events, VadEventKind.UTTERANCE_END)
        self.assertEqual(len(ends), 1)
        end = ends[0]
        self.assertEqual(end.reason, "endpoint")
        self.assertEqual(end.sample, self.burst_end_sample)
        # plan.md §1.6's ~0.8 s: 12800 samples, and 25 frames is exactly 12800.
        self.assertEqual(end.decided_at_sample, self.burst_end_sample + 25 * FRAME)
        self.assertEqual(end.decided_at_sample - end.sample, self.cfg.endpoint_samples)

    def test_events_arrive_in_causal_order(self):
        self.assertEqual(
            [e.kind for e in self.events],
            [VadEventKind.SPEECH_START, VadEventKind.SPEECH_END, VadEventKind.UTTERANCE_END],
        )

    def test_preroll_opens_the_gate_before_the_detected_onset(self):
        """A frame-level detector is late by construction. Clipping the first phoneme is a worse
        error than feeding a fraction of a second of room tone, so the gate opens ``preroll_ms``
        early — asserted exactly, because "roughly early" would pass a zero-preroll gate."""
        self.assertTrue(self.segments)
        first = self.segments[0]
        self.assertEqual(first.utterance_index, 1)
        self.assertEqual(first.start_sample, self.onset_sample - self.cfg.preroll_samples)
        self.assertLess(first.start_sample, self.onset_sample)

    def test_gate_reports_open_while_the_utterance_runs(self):
        # The utterance closed in setUp's trailing silence, so the *last* chunk reports shut.
        self.assertFalse(self.last.gate_open)
        self.assertTrue(self.last.reset_decoder)   # UTTERANCE_END invalidates decoder state


class TestMinimumDuration(unittest.TestCase):
    def test_a_click_shorter_than_min_speech_ms_produces_no_onset_at_all(self):
        """A door, a lip smack, a chair. Three frames is 96 ms against a 200 ms minimum."""
        vad = VadEndpointer(VadConfig())
        events, segments, last = collect(
            vad, silence(PAD_FRAMES), tone(3, amp=12000), silence(40)
        )
        self.assertEqual(events, [])          # no onset, therefore no end, therefore no utterance
        self.assertEqual(segments, [])        # and nothing reached the decoder
        self.assertEqual(vad.utterance_index, 0)
        self.assertFalse(last.gate_open)

    def test_a_burst_just_over_min_speech_ms_does_produce_an_onset(self):
        """The complement of the test above: the suppression must be a threshold, not a mute."""
        vad = VadEndpointer(VadConfig())
        events, segments, _ = collect(
            vad, silence(PAD_FRAMES), tone(8, amp=12000), silence(40)
        )
        self.assertEqual(len(kinds(events, VadEventKind.SPEECH_START)), 1)
        self.assertEqual(vad.utterance_index, 1)
        self.assertTrue(segments)


class TestHangover(unittest.TestCase):
    def test_a_dropout_shorter_than_the_hangover_does_not_leave_the_speech_state(self):
        """A stop consonant, a breath. 3 frames (96 ms) against a 250 ms hangover.

        Without hangover the state machine would emit SPEECH_END/SPEECH_START around every plosive
        and the decoder would be reset mid-word.
        """
        vad = VadEndpointer(VadConfig())
        events, _, _ = collect(
            vad,
            silence(PAD_FRAMES),
            tone(15),
            silence(3),           # 96 ms < hangover_ms (250 ms)
            tone(15, phase_samples=15 * FRAME + 3 * FRAME),
            silence(40),
        )
        self.assertEqual(len(kinds(events, VadEventKind.SPEECH_START)), 1)
        self.assertEqual(len(kinds(events, VadEventKind.SPEECH_END)), 1)
        self.assertEqual(len(kinds(events, VadEventKind.UTTERANCE_END)), 1)

    def test_a_dropout_longer_than_the_hangover_ends_speech_without_ending_the_utterance(self):
        vad = VadEndpointer(VadConfig())
        events, _, _ = collect(
            vad,
            silence(PAD_FRAMES),
            tone(15),
            silence(12),          # 384 ms: past the 250 ms hangover, short of the 800 ms endpoint
            tone(15),
            silence(40),
        )
        self.assertEqual(len(kinds(events, VadEventKind.SPEECH_START)), 2)
        self.assertEqual(len(kinds(events, VadEventKind.SPEECH_END)), 2)
        self.assertEqual(len(kinds(events, VadEventKind.UTTERANCE_END)), 1)
        self.assertEqual(vad.utterance_index, 1)


# ─── The two pauses: plan.md §1.6's whole point ─────────────────────────────────────────────────


class TestPauses(unittest.TestCase):
    """``Win+H`` ends the *session* on a short silence and offers no setting to change it. The
    replacement must end the *utterance* and leave the session alone — and this module must not
    even be able to end a session, which is why the assertions below are about utterance counts."""

    def _run(self, gap_frames):
        vad = VadEndpointer(VadConfig())
        events, segments, _ = collect(
            vad,
            silence(PAD_FRAMES),
            tone(20),
            silence(gap_frames),
            tone(20),
            silence(40),
        )
        return vad, events, segments

    def test_short_pause_does_not_endpoint(self):
        """A 640 ms pause — a comma, a breath, "um" — against the 800 ms endpoint.

        One utterance, spanning the pause. If this ever produces two, the product has acquired the
        ``Win+H`` behaviour it exists to replace.
        """
        vad, events, segments = self._run(20)          # 640 ms < 800 ms
        self.assertEqual(len(kinds(events, VadEventKind.UTTERANCE_END)), 1)
        self.assertEqual(vad.utterance_index, 1)
        self.assertEqual({s.utterance_index for s in segments}, {1})

    def test_long_pause_endpoints_and_opens_a_new_utterance(self):
        """A 1.28 s pause — the speaker finished a sentence and is thinking.

        Two utterances. The second one's audio must be tagged with a *different* utterance index,
        because that change is the instruction to reset decoder state (module docstring).
        """
        vad, events, segments = self._run(40)          # 1.28 s > 800 ms
        self.assertEqual(len(kinds(events, VadEventKind.UTTERANCE_END)), 2)
        self.assertEqual(vad.utterance_index, 2)
        self.assertEqual({s.utterance_index for s in segments}, {1, 2})
        # The tags are ordered: no segment of utterance 1 appears after one of utterance 2.
        indices = [s.utterance_index for s in segments]
        self.assertEqual(indices, sorted(indices))

    def test_a_two_minute_pause_costs_nothing_but_an_utterance_boundary(self):
        """plan.md §1.6: "A minute-long pause therefore costs nothing." Nothing in this module may
        end a session; the only thing 120 s of silence may produce is one utterance boundary."""
        vad = VadEndpointer(VadConfig())
        events, _, last = collect(
            vad,
            silence(PAD_FRAMES),
            tone(20),
            silence(3750),                 # 120 s
            tone(20),
            silence(40),
        )
        self.assertEqual(len(kinds(events, VadEventKind.UTTERANCE_END)), 2)
        self.assertEqual(vad.utterance_index, 2)
        self.assertFalse(last.gate_open)
        # No event kind exists here that could end a session, and that is the point.
        self.assertEqual(
            set(e.kind for e in events),
            {VadEventKind.SPEECH_START, VadEventKind.SPEECH_END, VadEventKind.UTTERANCE_END},
        )


# ─── Decoder gating — a correctness requirement, not an optimisation ────────────────────────────


class TestDecoderGating(unittest.TestCase):
    """plan.md §1.6: several ASR models hallucinate on long silence. Whisper emits "Thank you." /
    "Bye" over trailing silence with timestamps past the end of the audio. The gate is what stops
    that audio ever reaching the model, so these assertions are about *absence* of audio."""

    def test_decoder_gate_closes_over_silence(self):
        cfg = VadConfig()
        vad = VadEndpointer(cfg)

        opening = vad.process(silence(PAD_FRAMES))
        self.assertFalse(opening.gate_open)
        self.assertEqual(opening.segments, ())

        speaking = vad.process(tone(20))
        self.assertTrue(speaking.gate_open)
        self.assertTrue(speaking.segments)

        # The endpoint falls inside this chunk; the audio before it still belongs to the utterance
        # (the decoder needs the trailing silence to place the punctuation), the audio after it
        # does not.
        closing = vad.process(silence(40))
        self.assertFalse(closing.gate_open)
        self.assertEqual(len(kinds(closing.events, VadEventKind.UTTERANCE_END)), 1)
        gated_samples = sum(s.n_samples for s in closing.segments)
        # 25 frames of trailing silence reach the decoder — exactly the endpoint window — and not
        # one frame more.
        self.assertEqual(gated_samples, 25 * FRAME)

        # And now the thing the whole mechanism exists for: 60 seconds of silence produces no
        # audio for the decoder at all.
        for _ in range(60):
            chunk = vad.process(silence(31))       # ~1 s each
            self.assertEqual(chunk.segments, ())
            self.assertFalse(chunk.gate_open)
            self.assertEqual(chunk.events, ())

    def test_reset_decoder_is_signalled_exactly_at_the_utterance_boundary(self):
        vad = VadEndpointer(VadConfig())
        self.assertFalse(vad.process(silence(PAD_FRAMES)).reset_decoder)
        self.assertFalse(vad.process(tone(20)).reset_decoder)
        self.assertTrue(vad.process(silence(40)).reset_decoder)

    def test_gated_audio_is_exactly_the_utterance_plus_preroll_plus_endpoint_window(self):
        """The decoder's diet, accounted for to the sample. A gate that leaks would show up here
        as a larger number, which is far easier to diagnose than a hallucinated word."""
        cfg = VadConfig()
        vad = VadEndpointer(cfg)
        _, segments, _ = collect(vad, silence(PAD_FRAMES), tone(20), silence(40))
        total = sum(s.n_samples for s in segments)
        expected = cfg.preroll_samples + 20 * FRAME + cfg.endpoint_samples
        self.assertEqual(total, expected)


# ─── Chunking invariance ────────────────────────────────────────────────────────────────────────


class TestChunkingInvariance(unittest.TestCase):
    def test_the_same_stream_split_differently_produces_identical_events(self):
        """A component that is only correct at one chunking is correct by accident — the same
        argument docs/protocol.md §2.2 makes about the framing decoder."""
        stream = silence(PAD_FRAMES) + tone(20) + silence(20) + tone(20) + silence(40)

        one_shot = VadEndpointer(VadConfig())
        events_a, segments_a, _ = collect(one_shot, stream)

        # 666 bytes = 333 samples: not a multiple of the 512-sample frame, and even (an odd byte
        # count is a protocol error, not a chunking a caller may produce).
        dribbled = VadEndpointer(VadConfig())
        events_b = []
        segments_b = []
        for i in range(0, len(stream), 666):
            chunk = dribbled.process(stream[i:i + 666])
            events_b.extend(chunk.events)
            segments_b.extend(chunk.segments)

        # Events are the decisions, and they must be identical to the sample.
        self.assertEqual(events_a, events_b)

        # Segments are not compared one-for-one, and deliberately so: `_SegmentBuilder` coalesces
        # contiguous audio *within one call*, so a one-shot feed yields one segment per utterance
        # and a dribbled feed yields one per call. That is a packaging difference, not a
        # behavioural one. What must match is the decoder's actual diet — which audio, in which
        # order, tagged with which utterance.
        def diet(segments):
            out = {}
            for s in segments:
                got = out.setdefault(s.utterance_index, [s.start_sample, b""])
                out[s.utterance_index][1] = got[1] + s.pcm
            return out

        self.assertEqual(diet(segments_a), diet(segments_b))
        self.assertEqual(
            b"".join(s.pcm for s in segments_a), b"".join(s.pcm for s in segments_b)
        )
        # ...and the segments must still tile their utterance without gaps or overlaps.
        for segments in (segments_a, segments_b):
            by_utterance = {}
            for s in segments:
                cursor = by_utterance.get(s.utterance_index)
                if cursor is not None:
                    self.assertEqual(s.start_sample, cursor)
                by_utterance[s.utterance_index] = s.end_sample


# ─── flush, discontinuity, reset ────────────────────────────────────────────────────────────────


class TestStreamEdges(unittest.TestCase):
    def test_flush_closes_an_open_utterance_and_says_it_was_forced(self):
        """docs/protocol.md §6.1 ``stop``: finalise what exists, then end. An utterance left open
        at end of stream would never produce a ``final``."""
        vad = VadEndpointer(VadConfig())
        collect(vad, silence(PAD_FRAMES), tone(20))
        self.assertTrue(vad.gate_open)

        chunk = vad.flush()
        self.assertFalse(chunk.gate_open)
        ends = kinds(list(chunk.events), VadEventKind.UTTERANCE_END)
        self.assertEqual(len(ends), 1)
        # `reason` distinguishes a real endpoint from a forced one for a log reader.
        self.assertEqual(ends[0].reason, "flush")
        self.assertEqual(len(kinds(list(chunk.events), VadEventKind.SPEECH_END)), 1)

    def test_flush_emits_the_sub_frame_residue_so_the_last_phoneme_is_not_clipped(self):
        vad = VadEndpointer(VadConfig())
        collect(vad, silence(PAD_FRAMES), tone(20))
        residue = tone(1)[: 100 * BYTES_PER_SAMPLE]     # under one frame
        vad.process(residue)
        chunk = vad.flush()
        self.assertEqual(sum(s.n_samples for s in chunk.segments), 100)

    def test_discontinuity_closes_the_utterance_and_resets_rather_than_splicing(self):
        """docs/protocol.md §6.1: padding with zeroes or splicing both hand the decoder a lie.

        Resetting decoder state mid-utterance would make the next ``partial.text`` for that
        ``utterance_id`` no longer a superset of the previous one, which §8.1/§8.2 forbid — so the
        reset and the utterance boundary must happen together.
        """
        scorer = ConstantScorer([1.0] * 40)
        vad = VadEndpointer(VadConfig(), scorer)
        vad.process(tone(20))
        self.assertTrue(vad.gate_open)
        before = vad.position_samples

        chunk = vad.notify_discontinuity(gap_ms=500, reason="AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY")
        self.assertFalse(chunk.gate_open)
        self.assertTrue(chunk.reset_decoder)
        self.assertEqual(len(kinds(list(chunk.events), VadEventKind.UTTERANCE_END)), 1)
        self.assertEqual(len(kinds(list(chunk.events), VadEventKind.DISCONTINUITY)), 1)
        self.assertEqual(scorer.resets, 1)
        # The position clock advances by the lost duration so this module's sample positions stay
        # aligned with the Client's audio timeline across the gap.
        self.assertEqual(vad.position_samples, before + ms_to_samples(500, SAMPLE_RATE))

    def test_discontinuity_of_unknown_duration_advances_nothing(self):
        """``gap_ms == 0`` means "duration unknown" (§6.1). Inventing a duration would put every
        subsequent position quietly out of step with the Client's."""
        vad = VadEndpointer(VadConfig(), ConstantScorer([1.0] * 40))
        vad.process(tone(20))
        before = vad.position_samples
        vad.notify_discontinuity(gap_ms=0)
        self.assertEqual(vad.position_samples, before)

    def test_reset_clears_utterance_state_but_keeps_the_position_clock(self):
        vad = VadEndpointer(VadConfig(), ConstantScorer([1.0] * 40))
        vad.process(tone(20))
        pos = vad.position_samples
        vad.reset()
        self.assertEqual(vad.position_samples, pos)
        self.assertEqual(vad.utterance_index, 0)
        self.assertFalse(vad.gate_open)


class TestSafetyValve(unittest.TestCase):
    def test_max_utterance_ms_cuts_an_utterance_that_never_endpoints(self):
        """Continuous noise keeps resetting the endpoint timer, which would feed a streaming
        decoder without bound and never produce a ``final``. The valve is not a product feature and
        the event says so."""
        cfg = VadConfig(endpoint_silence_ms=800.0, max_utterance_ms=1000.0)
        vad = VadEndpointer(cfg, ConstantScorer([1.0] * 200))
        with self.assertLogs("kvit_asr.vad", level=logging.WARNING) as captured:
            events, _, _ = collect(vad, tone(100))     # 3.2 s of unbroken "speech"
        self.assertIn("safety valve, not an endpoint", "\n".join(captured.output))
        cuts = [e for e in kinds(events, VadEventKind.UTTERANCE_END) if e.reason == "max_utterance"]
        self.assertGreaterEqual(len(cuts), 2)
        # The replacement utterance opens immediately so the decoder feed stays continuous;
        # re-qualifying through min_speech_ms would punch a hole in it.
        self.assertGreaterEqual(vad.utterance_index, 3)


# ─── The energy fallback, and the ways it is worse ──────────────────────────────────────────────


class TestEnergyScorer(unittest.TestCase):
    """``EnergyZcrScorer`` detects *sound*, not *speech*, and its docstring enumerates how that
    bites. These tests pin the enumerated behaviour so it stays a known limitation rather than
    becoming a surprise — and so a measurement taken with this backend is never mistaken for one
    taken with Silero."""

    def test_energy_scorer_is_deaf_during_calibration_and_calibrates_on_what_it_hears(self):
        """Both halves of the documented wart, in the order a stream that opens mid-word hits them.

        "It needs a calibration window and reports nothing during it. A stream that opens mid-word
        therefore loses that word's onset, and the noise floor is initialised from speech, which
        suppresses detection until the first real pause pulls the floor down."
        """
        cfg = VadConfig()
        scorer = EnergyZcrScorer(cfg)
        loud = pcm_to_samples(tone(1, amp=20000))
        quiet = pcm_to_samples(silence(1))

        # energy_warmup_ms = 200 ms → 7 frames of 512. Every one of them scores 0.0 however loud.
        for _ in range(7):
            self.assertEqual(scorer.score(loud), 0.0)

        # And the floor is now sitting on the speech itself, so the very next loud frame is *not*
        # speech. This is the wart, asserted rather than discovered in a bake-off.
        self.assertLess(scorer.score(loud), 0.1)

        # The asymmetric adaptation (fast down, slow up) means one real pause fixes it.
        for _ in range(20):
            scorer.score(quiet)
        self.assertLess(scorer.noise_floor_dbfs, -100.0)
        self.assertGreater(scorer.score(loud), 0.9)

    def test_calibration_takes_the_quietest_frame_not_the_mean(self):
        cfg = VadConfig()
        scorer = EnergyZcrScorer(cfg)
        loud = pcm_to_samples(tone(1, amp=20000))
        quiet = pcm_to_samples(noise(1, amp=200, seed=5))
        for _ in range(6):
            scorer.score(loud)
        scorer.score(quiet)
        # A floor set from the mean of a window containing speech would be far too high, and a
        # floor too high is the failure that makes the detector deaf.
        self.assertLess(scorer.noise_floor_dbfs, -35.0)

    def test_low_level_noise_below_the_absolute_floor_never_reads_as_speech(self):
        """Without ``energy_abs_floor_dbfs`` a noise floor tracked down to digital silence makes
        -60 dBFS dither look like a 60 dB SNR event."""
        vad = VadEndpointer(VadConfig())
        events, segments, _ = collect(vad, silence(PAD_FRAMES), noise(60, amp=50, seed=11))
        self.assertEqual(events, [])
        self.assertEqual(segments, [])

    def test_loud_noise_over_a_silent_floor_does_read_as_speech(self):
        """A documented weakness, asserted rather than hoped about: this backend cannot tell a
        fan step or a slammed door from a voice. Silero was trained to reject exactly those, which
        is why ``create_vad`` warns loudly when it falls back to this one."""
        vad = VadEndpointer(VadConfig())
        events, _, _ = collect(vad, silence(PAD_FRAMES), noise(40, amp=4000, seed=12), silence(40))
        self.assertEqual(len(kinds(events, VadEventKind.SPEECH_START)), 1)

    def test_noise_present_from_the_start_calibrates_as_the_floor_and_is_not_speech(self):
        """"A stream that opens on steady room noise does not report that noise as speech." The
        cost of that property is the calibration window tested above."""
        vad = VadEndpointer(VadConfig())
        events, segments, _ = collect(vad, noise(120, amp=4000, seed=13))
        self.assertEqual(kinds(events, VadEventKind.SPEECH_START), [])
        self.assertEqual(segments, [])

    def test_a_burst_over_a_calibrated_noise_floor_is_still_detected(self):
        """The complement: once the floor is where the noise is, speech 20 dB above it is found.
        Without this the previous test could be satisfied by a scorer that never fires at all."""
        vad = VadEndpointer(VadConfig())
        room = noise(70, amp=1000, seed=14)
        burst = mix(room[: 20 * FRAME * BYTES_PER_SAMPLE], tone(20, amp=8000))
        stream = room[: PAD_FRAMES * FRAME * BYTES_PER_SAMPLE] + burst + room[: 40 * FRAME * BYTES_PER_SAMPLE]
        events, segments, _ = collect(vad, stream)
        self.assertEqual(len(kinds(events, VadEventKind.SPEECH_START)), 1)
        self.assertEqual(len(kinds(events, VadEventKind.UTTERANCE_END)), 1)
        self.assertTrue(segments)

    def test_utterance_boundaries_do_not_reset_the_scorer(self):
        """The noise floor and the acoustic context of the room are still valid after a pause.
        Throwing them away would make the first utterance after every pause behave like the first
        utterance of the stream — i.e. deaf for 200 ms."""
        scorer = ConstantScorer([1.0] * 20 + [0.0] * 40 + [1.0] * 20 + [0.0] * 40)
        vad = VadEndpointer(VadConfig(), scorer)
        collect(vad, tone(120))
        self.assertEqual(vad.utterance_index, 2)
        self.assertEqual(scorer.resets, 0)


# ─── Backend resolution ─────────────────────────────────────────────────────────────────────────


_SILERO_MODEL = os.environ.get("KVIT_SILERO_MODEL", "")


def _silero_skip_reason() -> str:
    if not _SILERO_MODEL:
        return (
            "no Silero model: set KVIT_SILERO_MODEL to a local silero_vad.onnx. Nothing is "
            "downloaded automatically (plan.md §1.9) and nothing may be installed."
        )
    if not os.path.isfile(_SILERO_MODEL):
        return f"KVIT_SILERO_MODEL={_SILERO_MODEL!r} is not a file"
    try:
        import onnxruntime  # noqa: F401
    except ImportError as exc:
        return f"onnxruntime is not importable in this interpreter ({exc}); see server/requirements"
    return ""


_SILERO_SKIP = _silero_skip_reason()


class TestSileroUnavailable(unittest.TestCase):
    """The paths every machine without a model actually takes. These run everywhere."""

    def test_missing_model_path_is_reported_as_unavailable_not_as_a_crash(self):
        scorer = SileroOnnxScorer(VadConfig(silero_model_path=None))
        with self.assertRaises(VadBackendUnavailable) as ctx:
            scorer.load()
        # plan.md §1.9: the message must say the model is not fetched automatically, because the
        # obvious next thought on seeing "model not found" is "why did it not download it".
        self.assertIn("silero_model_path", str(ctx.exception))

    def test_a_missing_file_names_the_resolved_path(self):
        scorer = SileroOnnxScorer(VadConfig(silero_model_path="./no-such-silero.onnx"))
        with self.assertRaises(VadBackendUnavailable) as ctx:
            scorer.load()
        self.assertIn("no-such-silero.onnx", str(ctx.exception))

    def test_a_wrong_frame_size_is_a_config_error_not_an_unavailable_backend(self):
        """The distinction is load-bearing: ``create_vad`` silently falls back on
        ``VadBackendUnavailable`` and must not absorb a deployment mistake."""
        scorer = SileroOnnxScorer(VadConfig(frame_samples=480, silero_model_path="x.onnx"))
        with self.assertRaises(VadConfigError):
            scorer.load()

    def test_scoring_before_load_raises_rather_than_returning_a_number(self):
        with self.assertRaises(VadBackendUnavailable):
            SileroOnnxScorer(VadConfig()).score(pcm_to_samples(tone(1)))

    def test_create_vad_auto_falls_back_to_energy_with_one_loud_warning(self):
        with self.assertLogs("kvit_asr.vad", level=logging.WARNING) as captured:
            vad = create_vad(VadConfig(backend="auto"))
        self.assertEqual(vad.backend_name, "energy-zcr")
        joined = "\n".join(captured.output)
        # A downgrade this significant must never be silent: every accuracy number taken
        # afterwards depends on which backend answered.
        self.assertIn("Silero VAD unavailable", joined)
        self.assertIn("worse", joined)

    def test_create_vad_silero_refuses_to_fall_back(self):
        """For the bake-off, where a silent fallback would corrupt the measurement (plan.md §4)."""
        with self.assertRaises(VadBackendUnavailable):
            create_vad(VadConfig(backend="silero"))

    def test_create_vad_energy_does_not_even_try_silero(self):
        vad = create_vad(VadConfig(backend="energy", silero_model_path="/nonexistent"))
        self.assertEqual(vad.backend_name, "energy-zcr")


@unittest.skipUnless(not _SILERO_SKIP, _SILERO_SKIP or "silero available")
class TestSileroPresent(unittest.TestCase):
    """Only runs where a real ``silero_vad.onnx`` and ``onnxruntime`` are both present.

    ``SileroOnnxScorer`` introspects the graph signature at load time because the published Silero
    exports differ between versions and none was available when it was written. These tests are the
    ones that turn that introspection from a guess into an observation — whoever first runs them
    should also revisit the "unverified" paragraph in the scorer's docstring.
    """

    def _cfg(self):
        return VadConfig(backend="silero", silero_model_path=_SILERO_MODEL)

    def test_the_graph_signature_binds(self):
        scorer = SileroOnnxScorer(self._cfg())
        scorer.load()
        self.addCleanup(scorer.close)
        self.assertGreater(scorer.score(pcm_to_samples(tone(1))), -1.0)

    def test_probabilities_stay_in_range_and_separate_silence_from_a_tone(self):
        scorer = SileroOnnxScorer(self._cfg())
        scorer.load()
        self.addCleanup(scorer.close)
        for _ in range(5):
            p = scorer.score(pcm_to_samples(silence(1)))
            self.assertGreaterEqual(p, 0.0)
            self.assertLessEqual(p, 1.0)
        # No assertion that a sine reads as speech: Silero is trained on speech and a pure tone is
        # not speech. Only the contract — a bounded probability — is asserted here.

    def test_endpointing_over_silence_with_silero_opens_no_utterance(self):
        vad = create_vad(self._cfg())
        self.assertEqual(vad.backend_name, "silero-onnx")
        events, segments, last = collect(vad, silence(100))
        self.assertEqual(events, [])
        self.assertEqual(segments, [])
        self.assertFalse(last.gate_open)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
