# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""The `sherpa_onnx_streaming` -> `backends.base.Backend` translation.

`kvit_asr.backends.sherpa_adapter`'s docstring names four translations. Each one is a place where
a plausible wrong answer produces no error anywhere, so each has a test here:

1. **reset reason -> lifecycle call.** Six ABC reasons onto four inner calls. Getting
   `UTTERANCE_BOUNDARY` wrong (calling `end_session`, say) would end the session at the first
   pause; getting `SESSION_START` wrong would never apply the vocabulary hints.
2. **cumulative text -> hypotheses.** One hypothesis per chunk, carrying the inner's cumulative
   text, emitted *even when unchanged* — the stability rule counts unchanged hypotheses, so
   suppressing repeats would starve it and nothing would ever commit.
3. **"nothing to finalise" -> None.** base.py reserves `None` for an utterance that never received
   a sample; the server turns it into `audio_underrun` rather than an empty `final`.
4. **inner error -> `decode_failed`.** `SherpaOnnxBackendError` is a plain `RuntimeError`, so
   without translation a failed decode is reported as a fatal `internal_error` and takes the
   connection down instead of just the session.

Everything runs against `FakeInner` — sherpa-onnx is not installed on this machine and the adapter
is the half of the pair that can be tested without it. What is *not* covered here, and cannot be
until a real run happens, is whether the inner's eight UNVERIFIED assumptions about sherpa-onnx's
API hold; that is the inner module's business and its own docstring lists them.
"""

from __future__ import annotations

import unittest

from kvit_asr.backends import load_backend
from kvit_asr.backends.base import (
    BackendDecodeError,
    ResetReason,
    StreamingMode,
    ms_to_bytes,
)
from kvit_asr.backends.sherpa_adapter import SherpaOnnxBackend, ZipformerBackend
from kvit_asr.config import BackendConfig


class FakeInner:
    """A stand-in with the inner lifecycle's exact surface and none of its dependencies."""

    mode = "native-streaming"
    supports_context = False

    def __init__(self, *, chunk_ms: int | None = 160, text: str = "hello world") -> None:
        self.model_id = "fake/model"
        self.chunk_ms = chunk_ms
        self.is_loaded = False
        self.config = None
        self.calls: list[tuple] = []
        self._text = text
        self._fed = 0
        self.raise_on_accept: Exception | None = None
        self.raise_on_finish: Exception | None = None

    def load(self, progress=None):
        self.calls.append(("load", progress))
        self.is_loaded = True

    def start_session(self, session_id, context=None, sample_rate=16000):
        self.calls.append(("start_session", session_id, tuple(context or ())))

    def accept_pcm(self, pcm: bytes) -> str:
        if self.raise_on_accept is not None:
            raise self.raise_on_accept
        self.calls.append(("accept_pcm", len(pcm)))
        self._fed += 1
        # Cumulative, and deliberately unchanging after the second chunk: the adapter must keep
        # emitting it anyway.
        return " ".join(self._text.split()[: min(self._fed, len(self._text.split()))])

    def finish_utterance(self) -> str:
        if self.raise_on_finish is not None:
            raise self.raise_on_finish
        self.calls.append(("finish_utterance",))
        return self._text

    def discard_utterance(self):
        self.calls.append(("discard_utterance",))

    def end_session(self):
        self.calls.append(("end_session",))

    def close(self):
        self.calls.append(("close",))
        self.is_loaded = False

    # -- helpers for assertions --------------------------------------------------------------

    def kinds(self) -> list[str]:
        return [c[0] for c in self.calls]


def _chunk(ms: int = 20) -> bytes:
    return b"\x01\x00" * (ms_to_bytes(ms) // 2)


class DeclarationTests(unittest.TestCase):
    def test_registry_resolves_both_names(self) -> None:
        self.assertIs(load_backend("sherpa-onnx"), SherpaOnnxBackend)
        self.assertIs(load_backend("zipformer"), ZipformerBackend)

    def test_capabilities_come_from_the_inner(self) -> None:
        b = SherpaOnnxBackend(FakeInner(chunk_ms=160))
        caps = b.capabilities
        self.assertEqual(caps.model_id, "fake/model")
        self.assertIs(caps.mode, StreamingMode.NATIVE_STREAMING)
        self.assertEqual(caps.natural_chunk_ms, 160)
        self.assertFalse(caps.supports_context)
        # A different chunk size is a different repository of weights, not a setter call.
        self.assertFalse(caps.chunk_ms_configurable)
        self.assertTrue(caps.requires_contiguous_audio)

    def test_unresolved_chunk_ms_reports_zero_rather_than_guessing(self) -> None:
        """Before load the model directory has not been read; 0 means "omit `ready.chunk_ms`"."""
        b = SherpaOnnxBackend(FakeInner(chunk_ms=None))
        self.assertEqual(b.capabilities.natural_chunk_ms, 0)
        self.assertNotIn("chunk_ms", b.capabilities.ready_fields())

    def test_context_support_is_never_invented(self) -> None:
        inner = FakeInner()
        inner.supports_context = True
        b = SherpaOnnxBackend(inner)
        self.assertTrue(b.capabilities.supports_context)
        self.assertTrue(b.set_context(["llama.cpp"]))

        inner.supports_context = False
        self.assertFalse(SherpaOnnxBackend(inner).set_context(["llama.cpp"]))


class ResetMappingTests(unittest.TestCase):
    """Translation 1: six ABC reasons onto four inner calls."""

    def setUp(self) -> None:
        self.inner = FakeInner()
        self.backend = SherpaOnnxBackend(self.inner)
        self.backend.load()
        self.inner.calls.clear()

    def test_session_start_opens_a_session_and_carries_context(self) -> None:
        self.backend.set_context(["llama.cpp", "protocol.h"])
        self.backend.reset(ResetReason.SESSION_START)
        self.assertEqual(self.inner.kinds(), ["start_session"])
        self.assertEqual(self.inner.calls[0][2], ("llama.cpp", "protocol.h"))

    def test_session_end_closes_the_session(self) -> None:
        self.backend.reset(ResetReason.SESSION_END)
        self.assertEqual(self.inner.kinds(), ["end_session"])

    def test_boundary_reasons_only_discard_the_utterance(self) -> None:
        """An utterance boundary must not end the session — plan.md §1.6 is the whole point."""
        for reason in (
            ResetReason.UTTERANCE_BOUNDARY,
            ResetReason.DISCONTINUITY,
            ResetReason.CANCEL,
            ResetReason.WARMUP,
        ):
            with self.subTest(reason=reason):
                self.inner.calls.clear()
                self.backend.reset(reason)
                self.assertEqual(self.inner.kinds(), ["discard_utterance"])

    def test_reset_before_load_touches_nothing(self) -> None:
        """Every inner call raises without a recognizer, and there is no state to drop yet."""
        inner = FakeInner()
        SherpaOnnxBackend(inner).reset(ResetReason.SESSION_START)
        self.assertEqual(inner.calls, [])

    def test_session_ids_are_distinct_per_session(self) -> None:
        self.backend.reset(ResetReason.SESSION_START)
        self.backend.reset(ResetReason.SESSION_END)
        self.backend.reset(ResetReason.SESSION_START)
        ids = [c[1] for c in self.inner.calls if c[0] == "start_session"]
        self.assertEqual(len(ids), 2)
        self.assertEqual(len(set(ids)), 2)


class StreamingTests(unittest.TestCase):
    """Translations 2 and 3."""

    def setUp(self) -> None:
        self.inner = FakeInner(text="the café bill")
        self.backend = SherpaOnnxBackend(self.inner)
        self.backend.load()
        self.backend.reset(ResetReason.SESSION_START)

    def test_one_cumulative_hypothesis_per_chunk(self) -> None:
        first = self.backend.feed(_chunk())
        second = self.backend.feed(_chunk())
        self.assertEqual(len(first), 1)
        self.assertEqual(len(second), 1)
        self.assertEqual(first[0].text, "the")
        self.assertEqual(second[0].text, "the café")
        # Cumulative, not a delta (docs/protocol.md §8.1).
        self.assertTrue(second[0].text.startswith(first[0].text))
        self.assertFalse(first[0].is_final)

    def test_unchanged_text_is_still_emitted(self) -> None:
        """The stability rule counts *unchanged* hypotheses; suppressing repeats starves it."""
        for _ in range(6):
            self.backend.feed(_chunk())
        last = self.backend.feed(_chunk())
        self.assertEqual(len(last), 1)
        self.assertEqual(last[0].text, "the café bill")

    def test_empty_payload_produces_nothing(self) -> None:
        self.assertEqual(self.backend.feed(b""), [])
        self.assertNotIn("accept_pcm", self.inner.kinds())

    def test_audio_position_counts_the_bytes_actually_fed(self) -> None:
        h = self.backend.feed(_chunk(100))[0]
        self.assertEqual(h.audio_pos_ms, 100)
        h = self.backend.feed(_chunk(100))[0]
        self.assertEqual(h.audio_pos_ms, 200)

    def test_audio_position_survives_an_utterance_boundary(self) -> None:
        """It is a position in the *session* timeline; utterances do not restart it."""
        self.backend.feed(_chunk(100))
        self.backend.finalize()
        self.backend.reset(ResetReason.UTTERANCE_BOUNDARY)
        h = self.backend.feed(_chunk(100))[0]
        self.assertEqual(h.audio_pos_ms, 200)

    def test_audio_position_restarts_with_the_session(self) -> None:
        self.backend.feed(_chunk(100))
        self.backend.reset(ResetReason.SESSION_END)
        self.backend.reset(ResetReason.SESSION_START)
        h = self.backend.feed(_chunk(100))[0]
        self.assertEqual(h.audio_pos_ms, 100)

    def test_finalize_returns_the_final_hypothesis(self) -> None:
        self.backend.feed(_chunk())
        final = self.backend.finalize()
        self.assertIsNotNone(final)
        self.assertTrue(final.is_final)
        self.assertEqual(final.text, "the café bill")

    def test_finalize_with_no_audio_returns_none(self) -> None:
        """base.py reserves None for "nothing to finalise" -> the server reports audio_underrun."""
        self.assertIsNone(self.backend.finalize())
        self.assertNotIn("finish_utterance", self.inner.kinds())

    def test_finalize_twice_returns_none_the_second_time(self) -> None:
        self.backend.feed(_chunk())
        self.assertIsNotNone(self.backend.finalize())
        self.assertIsNone(self.backend.finalize())

    def test_feed_before_load_is_a_state_error(self) -> None:
        from kvit_asr.backends.base import BackendStateError

        with self.assertRaises(BackendStateError):
            SherpaOnnxBackend(FakeInner()).feed(_chunk())

    def test_odd_length_payload_is_rejected(self) -> None:
        from kvit_asr.backends.base import BackendError

        with self.assertRaises(BackendError):
            self.backend.feed(b"\x00\x00\x00")


class DecodeErrorTests(unittest.TestCase):
    """Translation 4: a plain RuntimeError from the inner must not kill the connection."""

    def setUp(self) -> None:
        self.inner = FakeInner()
        self.backend = SherpaOnnxBackend(self.inner)
        self.backend.load()
        self.backend.reset(ResetReason.SESSION_START)

    def test_accept_failure_becomes_a_non_fatal_decode_error(self) -> None:
        self.inner.raise_on_accept = RuntimeError("is_ready never went false")
        with self.assertRaises(BackendDecodeError) as caught:
            self.backend.feed(_chunk())
        self.assertEqual(caught.exception.wire_code, "decode_failed")
        self.assertFalse(caught.exception.fatal)
        self.assertIn("is_ready never went false", str(caught.exception))

    def test_finish_failure_becomes_a_non_fatal_decode_error(self) -> None:
        self.backend.feed(_chunk())
        self.inner.raise_on_finish = RuntimeError("accept_waveform rejected the samples")
        with self.assertRaises(BackendDecodeError):
            self.backend.finalize()

    def test_a_backend_error_keeps_its_own_wire_code(self) -> None:
        from kvit_asr.backends.base import BackendLoadError

        self.inner.raise_on_accept = BackendLoadError("out of memory")
        with self.assertRaises(BackendLoadError):
            self.backend.feed(_chunk())


class WarmUpTests(unittest.TestCase):
    def test_skipped_when_the_inner_warms_itself(self) -> None:
        inner = FakeInner()
        inner.config = type("Cfg", (), {"warmup_ms": 300, "decoding_method": "greedy_search",
                                        "provider": "cpu", "precision": "auto"})()
        b = SherpaOnnxBackend(inner)
        b.load()
        inner.calls.clear()
        b.warm_up()
        self.assertNotIn("accept_pcm", inner.kinds())
        self.assertIsNotNone(b.warm_ms)

    def test_base_behaviour_when_the_inner_does_not(self) -> None:
        inner = FakeInner()
        inner.config = type("Cfg", (), {"warmup_ms": 0, "decoding_method": "greedy_search",
                                        "provider": "cpu", "precision": "auto"})()
        b = SherpaOnnxBackend(inner)
        b.load()
        inner.calls.clear()
        b.warm_up(audio_ms=320)
        self.assertIn("accept_pcm", inner.kinds())


class FromBackendConfigTests(unittest.TestCase):
    """The command line's route in. Constructed only — never loaded — so no sherpa-onnx needed."""

    def _cfg(self, **fields) -> BackendConfig:
        fields.setdefault("model", "csukuangfj2/sherpa-onnx-nemotron-...-160ms-2026-04-25")
        return BackendConfig(id="sherpa-onnx", **fields)

    def test_model_chunk_and_decoding_reach_the_inner_config(self) -> None:
        b = SherpaOnnxBackend.from_backend_config(
            self._cfg(chunk_ms=160, decoding="greedy_search", device="cuda")
        )
        cfg = b.inner.config
        self.assertEqual(cfg.chunk_ms, 160)
        self.assertEqual(cfg.decoding_method, "greedy_search")
        self.assertEqual(cfg.provider, "cuda")

    def test_auto_device_is_left_to_the_backend(self) -> None:
        """CANDIDATES.md §6.1 makes CPU the baseline; `load()` logs what was actually available."""
        b = SherpaOnnxBackend.from_backend_config(self._cfg(device="auto"))
        self.assertEqual(b.inner.config.provider, "cpu")

    def test_backend_opt_wins(self) -> None:
        b = SherpaOnnxBackend.from_backend_config(
            self._cfg(chunk_ms=160, options={"num_threads": "8", "precision": "int8"})
        )
        self.assertEqual(b.inner.config.num_threads, 8)
        self.assertEqual(b.inner.config.precision, "int8")

    def test_dml_is_refused_rather_than_guessed(self) -> None:
        with self.assertRaises(ValueError) as caught:
            SherpaOnnxBackend.from_backend_config(self._cfg(device="dml"))
        self.assertIn("DirectML", str(caught.exception))

    def test_unknown_backend_opt_is_rejected(self) -> None:
        with self.assertRaises(ValueError) as caught:
            SherpaOnnxBackend.from_backend_config(self._cfg(options={"numthreads": "8"}))
        self.assertIn("numthreads", str(caught.exception))

    def test_missing_model_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            SherpaOnnxBackend.from_backend_config(BackendConfig(id="sherpa-onnx"))


class ZipformerTests(unittest.TestCase):
    def test_default_decoding_is_the_biasing_one(self) -> None:
        b = ZipformerBackend.from_backend_config(
            BackendConfig(id="zipformer", model="sherpa-onnx-streaming-zipformer-en-2023-06-26")
        )
        self.assertEqual(b.inner.config.decoding_method, "modified_beam_search")
        self.assertTrue(b.capabilities.supports_context)

    def test_greedy_search_narrows_context_support(self) -> None:
        """Same model measured context-free. Claiming biasing here would mis-file the run."""
        b = ZipformerBackend.from_backend_config(
            BackendConfig(
                id="zipformer",
                model="sherpa-onnx-streaming-zipformer-en-2023-06-26",
                decoding="greedy_search",
            )
        )
        self.assertEqual(b.inner.config.decoding_method, "greedy_search")
        self.assertFalse(b.capabilities.supports_context)

    def test_the_base_backend_still_refuses_modified_beam_search(self) -> None:
        """`--backend sherpa-onnx --decoding modified_beam_search` must stay impossible.

        models.json said exactly that until this change. It resolves now, but it must still fail —
        loudly, naming the alternative — rather than quietly producing an unbiased run in the
        context-enabled column of plan.md §4.
        """
        from kvit_asr.backends.sherpa_onnx_streaming import SherpaOnnxBackendError

        b = SherpaOnnxBackend.from_backend_config(
            BackendConfig(id="sherpa-onnx", model="x", decoding="modified_beam_search")
        )
        with self.assertRaises(SherpaOnnxBackendError) as caught:
            b.inner._validate_decoding_method()
        self.assertIn("zipformer", str(caught.exception))


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
