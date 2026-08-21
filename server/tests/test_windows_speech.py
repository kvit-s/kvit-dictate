"""`--backend windows-speech`: the process seam, not the model.

Every test here runs a real child process — `fake_speech_helper.py` under this interpreter —
because that is the part of this backend that can go wrong in ways worth catching. The model
itself is Microsoft's and is exercised by hand; what is covered here is what happens around it:

1. **Startup is legible.** A helper that reports an uninstalled model produces a
   ``BackendLoadError`` carrying the reason, not a traceback and not a silent hang.
2. **The window reaches the helper intact**, and `finalize_only` really does mean one decode per
   utterance rather than one per hop.
3. **The silence gate holds.** This model is Whisper and it invents sentences over silence
   (measured: two seconds of digital silence returned fluent Welsh). A silent window must never
   reach the helper at all.
4. **A wedged or dead helper becomes a timeout with a message**, and never an audio path that
   blocks forever — `feed()` is synchronous, so a hung read would stall a dictation session with
   the user still talking.
5. **A desynchronised stream is refused** rather than turned into a huge allocation.

Nothing here needs .NET, the Windows App SDK, or the speech model, so it runs anywhere.
"""

from __future__ import annotations

import os
import sys
import time
import unittest
from pathlib import Path

from kvit_asr.backends import create_backend, load_backend
from kvit_asr.backends.base import BackendDecodeError, BackendLoadError, ResetReason
from kvit_asr.backends.windows_speech import WindowsSpeechBackend, find_helper

_FAKE = str(Path(__file__).resolve().parent / "fake_speech_helper.py")

# 16 kHz mono s16le. A flat non-zero level is enough to clear the silence gate; the fake helper
# never looks at the samples.
def _speech(ms: int) -> bytes:
    return b"\x00\x40" * (16 * ms)


def _silence(ms: int) -> bytes:
    return b"\x00\x00" * (16 * ms)


def _backend(*helper_args: str, **kwargs) -> WindowsSpeechBackend:
    kwargs.setdefault("startup_timeout_s", 20.0)
    kwargs.setdefault("timeout_s", 20.0)
    return WindowsSpeechBackend(
        exe=sys.executable, helper_args=[_FAKE, *helper_args], **kwargs
    )


class _Loaded:
    """Loads a backend for the body of a test and always unloads it, including on failure."""

    def __init__(self, backend: WindowsSpeechBackend) -> None:
        self.backend = backend

    def __enter__(self) -> WindowsSpeechBackend:
        self.backend.load()
        return self.backend

    def __exit__(self, *exc) -> None:
        self.backend.unload()


class TestRegistration(unittest.TestCase):
    def test_the_registry_row_resolves(self):
        self.assertIs(load_backend("windows-speech"), WindowsSpeechBackend)

    def test_it_is_constructible_without_the_helper_or_the_model(self):
        # The module is stdlib-only, so importing and constructing must work on any machine. Only
        # load() may fail for a missing helper.
        b = create_backend("windows-speech")
        self.assertEqual(b.capabilities.model_id, "windows-ai-speech")
        self.assertEqual(b.capabilities.mode.value, "pseudo-streaming")


class TestStartup(unittest.TestCase):
    def test_ready_frame_loads_the_backend(self):
        with _Loaded(_backend("--ready")) as b:
            self.assertTrue(b.is_loaded)
            self.assertEqual(b.describe()["native_folder"], r"C:\fake\winml")

    def test_an_uninstalled_model_names_itself(self):
        b = _backend("--fail", "the speech model is not installed (state: NotReady)")
        with self.assertRaises(BackendLoadError) as caught:
            b.load()
        self.assertIn("not installed", str(caught.exception))
        b.unload()

    def test_a_helper_that_never_answers_times_out(self):
        b = _backend("--silent", startup_timeout_s=1.0)
        t0 = time.perf_counter()
        with self.assertRaises(BackendLoadError):
            b.load()
        # The point is that it returns at all; the bound keeps a regression to a hang honest.
        self.assertLess(time.perf_counter() - t0, 15.0)
        b.unload()

    def test_a_missing_executable_says_how_to_build_it(self):
        b = WindowsSpeechBackend(exe=str(Path(__file__).parent / "no-such-helper.exe"))
        with self.assertRaises(BackendLoadError) as caught:
            b.load()
        self.assertIn("dotnet build", str(caught.exception))

    def test_a_desynchronised_stream_is_refused(self):
        b = _backend("--garbage-length", startup_timeout_s=5.0)
        with self.assertRaises(BackendLoadError) as caught:
            b.load()
        self.assertIn("desynchronised", str(caught.exception))
        b.unload()


class TestTranscription(unittest.TestCase):
    def test_finalize_only_decodes_once_per_utterance(self):
        with _Loaded(_backend("--ready")) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(20):                       # 2 s, in 100 ms chunks
                self.assertEqual(b.feed(_speech(100)), [], "finalize-only must not emit partials")
            final = b.finalize()
            self.assertIsNotNone(final)
            self.assertTrue(final.is_final)
            self.assertEqual(final.text, "the fake helper heard something")
            self.assertEqual(b.describe()["requests"], 1)

    def test_the_whole_utterance_reaches_the_helper(self):
        with _Loaded(_backend("--echo-length")) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(30):                       # 3 s
                b.feed(_speech(100))
            self.assertEqual(b.finalize().text, "3000 ms")

    def test_the_window_bounds_what_is_sent(self):
        with _Loaded(_backend("--echo-length", window_ms=2000)) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(50):                       # 5 s into a 2 s window
                b.feed(_speech(100))
            self.assertEqual(b.finalize().text, "2000 ms")

    def test_a_hop_emits_partials_when_finalize_only_is_off(self):
        with _Loaded(_backend("--echo-length", finalize_only=False, hop_ms=500, window_ms=10000)) as b:
            b.reset(ResetReason.SESSION_START)
            emitted = []
            for _ in range(20):                       # 2 s at a 500 ms hop
                emitted += b.feed(_speech(100))
            self.assertGreaterEqual(len(emitted), 3)
            self.assertGreater(b.describe()["requests"], 1)

    def test_utterances_do_not_bleed_into_each_other(self):
        with _Loaded(_backend("--echo-length")) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(10):
                b.feed(_speech(100))
            self.assertEqual(b.finalize().text, "1000 ms")
            b.reset(ResetReason.UTTERANCE_BOUNDARY)
            for _ in range(20):
                b.feed(_speech(100))
            self.assertEqual(b.finalize().text, "2000 ms")


class TestSilenceGate(unittest.TestCase):
    """Whisper invents over silence. A silent window must not reach the model at all."""

    def test_silence_never_reaches_the_helper(self):
        with _Loaded(_backend("--ready")) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(20):
                b.feed(_silence(100))
            final = b.finalize()
            self.assertEqual("" if final is None else final.text, "")
            d = b.describe()
            self.assertEqual(d["requests"], 0, "a silent window was sent to the model")
            self.assertGreaterEqual(d["silence_skipped"], 1)

    def test_speech_still_gets_through(self):
        with _Loaded(_backend("--ready")) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(10):
                b.feed(_speech(100))
            self.assertEqual(b.finalize().text, "the fake helper heard something")
            self.assertEqual(b.describe()["requests"], 1)

    def test_the_gate_can_be_opened_for_a_test_but_is_not_open_by_default(self):
        self.assertGreater(WindowsSpeechBackend()._silence_peak_threshold, 0)
        with _Loaded(_backend("--echo-length", silence_peak_threshold=0)) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(10):
                b.feed(_silence(100))
            self.assertEqual(b.finalize().text, "1000 ms")


class TestWarmUp(unittest.TestCase):
    """The inherited warm-up feeds silence, which this backend's own gate refuses."""

    def test_warm_up_actually_decodes_something(self):
        with _Loaded(_backend("--echo-length")) as b:
            ms = b.warm_up(audio_ms=300)
            self.assertGreaterEqual(b.describe()["requests"], 1,
                                    "warm-up did not reach the model")
            self.assertEqual(b.describe()["silence_skipped"], 0,
                             "warm-up audio was mistaken for silence")
            self.assertGreaterEqual(ms, 0)

    def test_warm_up_clears_a_high_gate_too(self):
        with _Loaded(_backend("--echo-length", silence_peak_threshold=20000)) as b:
            b.warm_up(audio_ms=200)
            self.assertGreaterEqual(b.describe()["requests"], 1)

    def test_warm_up_leaves_no_text_behind(self):
        with _Loaded(_backend("--text", "warmup noise")) as b:
            b.warm_up(audio_ms=200)
            b.reset(ResetReason.SESSION_START)
            self.assertIsNone(b.finalize(), "warm-up audio leaked into the next utterance")


class TestFailureDuringDictation(unittest.TestCase):
    def test_a_decode_error_names_the_reason_and_does_not_kill_the_backend(self):
        with _Loaded(_backend("--error-on-audio", "the model said no")) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(10):
                b.feed(_speech(100))
            with self.assertRaises(BackendDecodeError) as caught:
                b.finalize()
            self.assertIn("the model said no", str(caught.exception))
            # Still usable: a bad window is not a dead session.
            b.reset(ResetReason.UTTERANCE_BOUNDARY)
            for _ in range(10):
                b.feed(_speech(100))
            with self.assertRaises(BackendDecodeError):
                b.finalize()

    def test_a_wedged_helper_becomes_a_timeout_not_a_hang(self):
        with _Loaded(_backend("--hang-on-audio", timeout_s=1.0)) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(10):
                b.feed(_speech(100))
            t0 = time.perf_counter()
            with self.assertRaises(BackendDecodeError) as caught:
                b.finalize()
            self.assertLess(time.perf_counter() - t0, 15.0)
            self.assertIn("did not answer", str(caught.exception))

    def test_a_helper_that_dies_mid_decode_says_so(self):
        with _Loaded(_backend("--die-on-audio")) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(10):
                b.feed(_speech(100))
            with self.assertRaises(BackendDecodeError) as caught:
                b.finalize()
            self.assertIn("helper", str(caught.exception))

    def test_a_dead_helper_stays_dead_rather_than_desynchronising(self):
        # After a timeout the answer may still be in flight, so reusing the link would hand a late
        # frame to the next caller as its own. The second call must fail too.
        with _Loaded(_backend("--hang-on-audio", timeout_s=1.0)) as b:
            b.reset(ResetReason.SESSION_START)
            for _ in range(10):
                b.feed(_speech(100))
            with self.assertRaises(BackendDecodeError):
                b.finalize()
            b.reset(ResetReason.UTTERANCE_BOUNDARY)
            for _ in range(10):
                b.feed(_speech(100))
            with self.assertRaises(BackendDecodeError) as caught:
                b.finalize()
            self.assertIn("no longer usable", str(caught.exception))


class TestConfigPlumbing(unittest.TestCase):
    class _Cfg:
        def __init__(self, **kw):
            self.chunk_ms = kw.pop("chunk_ms", None)
            self.options = kw

    def test_command_line_strings_are_coerced(self):
        b = WindowsSpeechBackend.from_backend_config(
            self._Cfg(chunk_ms=500, finalize_only="false", window_ms="12000",
                      silence_peak_threshold="0", timeout_s="7.5")
        )
        self.assertEqual(b.hop_ms, 500)
        self.assertEqual(b.window_ms, 12000)
        self.assertEqual(b._silence_peak_threshold, 0)
        self.assertEqual(b._timeout_s, 7.5)

    def test_finalize_only_forces_the_hop_past_the_window(self):
        b = WindowsSpeechBackend.from_backend_config(
            self._Cfg(chunk_ms=1000, finalize_only="true", window_ms="9000")
        )
        self.assertGreaterEqual(b.hop_ms, b.window_ms)

    def test_finalize_only_is_the_default(self):
        self.assertGreaterEqual(WindowsSpeechBackend().hop_ms, WindowsSpeechBackend().window_ms)


class TestHelperDiscovery(unittest.TestCase):
    def test_the_environment_variable_wins(self):
        old = os.environ.get("KVIT_SPEECH_EXE")
        os.environ["KVIT_SPEECH_EXE"] = sys.executable
        try:
            self.assertEqual(find_helper(), Path(sys.executable))
        finally:
            if old is None:
                del os.environ["KVIT_SPEECH_EXE"]
            else:
                os.environ["KVIT_SPEECH_EXE"] = old

    def test_a_bad_environment_variable_is_not_a_crash(self):
        old = os.environ.get("KVIT_SPEECH_EXE")
        os.environ["KVIT_SPEECH_EXE"] = str(Path(__file__).parent / "nope.exe")
        try:
            self.assertIsNone(find_helper())
        finally:
            if old is None:
                del os.environ["KVIT_SPEECH_EXE"]
            else:
                os.environ["KVIT_SPEECH_EXE"] = old


if __name__ == "__main__":
    unittest.main()
