# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Unit tests for `bench/ab_service.py`.

Only the pure parts: the service spec parser, the truncation marker, the non-speech gap finder
and the two guards. Nothing here opens a socket or reads the corpus — a test that needs the
operator's H: drive is a test that fails on every other machine.
"""

from __future__ import annotations

import json
import sys
import unittest
import wave
from pathlib import Path
from tempfile import TemporaryDirectory

BENCH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BENCH_DIR))
sys.path.insert(0, str(BENCH_DIR.parent / "server"))

import ab_service as ab  # noqa: E402


class ServiceSpec(unittest.TestCase):
    def test_name_and_url(self) -> None:
        s = ab.Service.parse("qwen=http://box.lan:10301")
        self.assertEqual(s.name, "qwen")
        self.assertEqual(s.base_url, "http://box.lan:10301")
        self.assertEqual(s.language, "en")
        self.assertIsNone(s.model)

    def test_trailing_slash_is_dropped(self) -> None:
        # The path is appended verbatim, so a kept slash would post to `//v1/...`.
        self.assertEqual(ab.Service.parse("a=http://x/").base_url, "http://x")

    def test_options(self) -> None:
        s = ab.Service.parse("r2t2=http://x:1,model=vendor/Model-1,language=none")
        self.assertEqual(s.model, "vendor/Model-1")
        self.assertIsNone(s.language, "language=none must mean auto-detect, not the string")

    def test_a_spec_without_a_url_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            ab.Service.parse("justaname")


class TruncationMark(unittest.TestCase):
    def test_detects_and_strips(self) -> None:
        self.assertTrue(ab.is_truncated("hello there|"))
        self.assertTrue(ab.is_truncated("hello there|  "))
        self.assertEqual(ab.strip_mark("hello there|"), "hello there")

    def test_a_pipe_inside_the_text_is_not_a_truncation(self) -> None:
        self.assertFalse(ab.is_truncated("pipe the output | to grep"))
        self.assertEqual(ab.strip_mark("pipe the output | to grep"),
                         "pipe the output | to grep")

    def test_empty(self) -> None:
        self.assertFalse(ab.is_truncated(""))
        self.assertEqual(ab.strip_mark(""), "")


def _utt(start_ms: int, end_ms: int) -> ab.Utterance:
    return ab.Utterance(session="s", utt=1, start_ms=start_ms, end_ms=end_ms, audio=b"")


class NonSpeechGaps(unittest.TestCase):
    #: 10 s of audio, so the arithmetic below is readable.
    PCM = b"\x00\x00" * (ab.SAMPLE_RATE * 10)

    def test_gap_before_first_and_after_last(self) -> None:
        clips = ab.nonspeech_clips("s", self.PCM, [_utt(3000, 6000)],
                                   min_ms=1000, max_ms=15000)
        self.assertEqual([(c.start_ms, c.end_ms) for c in clips], [(0, 3000), (6000, 10000)])
        self.assertTrue(all(c.kind == "nonspeech" for c in clips))

    def test_gaps_shorter_than_min_are_not_probed(self) -> None:
        # A 500 ms pause is a breath, not an open microphone; asking a model about it measures
        # nothing and costs a request.
        clips = ab.nonspeech_clips("s", self.PCM, [_utt(500, 9500)],
                                   min_ms=1000, max_ms=15000)
        self.assertEqual(clips, [])

    def test_a_long_gap_is_capped_not_dropped(self) -> None:
        clips = ab.nonspeech_clips("s", self.PCM, [_utt(9000, 10000)],
                                   min_ms=1000, max_ms=2000)
        self.assertEqual([(c.start_ms, c.end_ms) for c in clips], [(0, 2000)])

    def test_overlapping_utterances_do_not_invent_a_gap(self) -> None:
        # The endpointer can hand back segments whose padding overlaps. Sorting by start and
        # carrying the cursor forward with max() is what stops a negative-length gap.
        clips = ab.nonspeech_clips("s", self.PCM, [_utt(1000, 6000), _utt(4000, 5000)],
                                   min_ms=500, max_ms=15000)
        self.assertEqual([(c.start_ms, c.end_ms) for c in clips], [(0, 1000), (6000, 10000)])

    def test_clip_audio_matches_its_bounds(self) -> None:
        clips = ab.nonspeech_clips("s", self.PCM, [_utt(4000, 5000)],
                                   min_ms=1000, max_ms=15000)
        for c in clips:
            self.assertEqual(c.duration_ms, c.end_ms - c.start_ms)
            self.assertEqual(len(c.audio),
                             (c.end_ms - c.start_ms) * ab.SAMPLE_RATE * ab.BYTES_PER_SAMPLE // 1000)


class Segmentation(unittest.TestCase):
    def test_speech_is_found_and_silence_is_not(self) -> None:
        # A burst of loud noise between two stretches of quiet. The energy scorer needs its
        # warm-up window of quiet first, which is why the clip does not open on the burst.
        import math
        import struct
        sr = ab.SAMPLE_RATE
        samples = []
        for i in range(sr * 6):
            t = i / sr
            loud = 1.0 < t < 3.0
            samples.append(int(12000 * math.sin(2 * math.pi * 220 * t)) if loud else 0)
        pcm = struct.pack(f"<{len(samples)}h", *samples)
        utts = ab.segment_session("s", pcm, endpoint_silence_ms=800.0)
        self.assertEqual(len(utts), 1, "one burst of tone is one utterance")
        u = utts[0]
        self.assertLess(u.start_ms, 1100, "preroll should reach back to before the onset")
        self.assertGreater(u.end_ms, 2900)
        self.assertGreater(len(u.audio), 0)

    def test_pure_silence_yields_nothing(self) -> None:
        self.assertEqual(ab.segment_session("s", b"\x00\x00" * (ab.SAMPLE_RATE * 5),
                                            endpoint_silence_ms=800.0), [])


class ShippedText(unittest.TestCase):
    def test_only_finals_in_order(self) -> None:
        with TemporaryDirectory() as td:
            p = Path(td) / "events.jsonl"
            p.write_text("\n".join(json.dumps(o) for o in [
                {"t": "start", "service": "http://x"},
                {"t": "partial", "text": "one"},
                {"t": "final", "text": "One."},
                {"t": "normalize", "text": "ignored"},
                {"t": "final", "text": "Two."},
                {"t": "final", "text": "   "},
            ]), encoding="utf-8")
            self.assertEqual(ab.shipped_text(p), "One. Two.")

    def test_a_malformed_line_is_skipped_not_fatal(self) -> None:
        with TemporaryDirectory() as td:
            p = Path(td) / "events.jsonl"
            p.write_text('{"t":"final","text":"A."}\nnot json\n{"t":"final","text":"B."}\n',
                         encoding="utf-8")
            self.assertEqual(ab.shipped_text(p), "A. B.")


class PrivacyGuards(unittest.TestCase):
    def test_output_dir_gets_its_own_gitignore(self) -> None:
        # bench/results/**/*.md is un-ignored by the repository's .gitignore, which would commit
        # adjudicate.md and the operator's dictation with it. This is the guard against that.
        with TemporaryDirectory() as td:
            out = Path(td)
            ab.protect_output_dir(out)
            body = (out / ".gitignore").read_text(encoding="utf-8")
            self.assertIn("\n*\n", body)
            self.assertIn("!*.aggregate.json", body)
            self.assertNotIn("!*.md", body)

    def test_transcript_bearing_aggregate_is_refused(self) -> None:
        with self.assertRaises(AssertionError):
            ab.assert_no_verbatim({"per_service": {"qwen": {"answers": {"a": "hello"}}}})
        with self.assertRaises(AssertionError):
            ab.assert_no_verbatim({"rows": [{"ok": 1}, {"text": "hello"}]})

    def test_a_clean_aggregate_passes(self) -> None:
        ab.assert_no_verbatim({"per_service": {"qwen": {"latency_ms": {"mean": 1.0}}},
                               "worst_sessions": [["20260101-000000-s1", 0.2]]})


class WordCount(unittest.TestCase):
    def test_counts_whitespace_separated_runs(self) -> None:
        self.assertEqual(ab.word_count("one two  three"), 3)
        self.assertEqual(ab.word_count("   "), 0)


if __name__ == "__main__":
    unittest.main()
