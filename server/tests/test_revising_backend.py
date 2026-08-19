"""A growing-window batch model revises its own published text, and the session survives it.

This is the failure that ended two real dictations against `qwen-asr-http` (progress.md §3). The
mechanism, end to end:

1. The backend re-transcribes the **whole utterance** on every hop. Once it can see where the
   sentence ended it re-cases the opening word and inserts the punctuation it was not yet sure of,
   so `so I was thinking we` comes back as `So I was thinking, we`.
2. `StabilityTracker` has already published a mark covering those bytes. §8.2 binds the new
   hypothesis against the *previously published* mark, so it raises `STABLE_PREFIX_MUTATED` with
   `diverge_offset=0` — correctly, and with no value of `stable_bytes` available that would have
   made the message conforming.
3. `Session._fail_session` turns that into `error{code:decode_failed, fatal:false}` + `stopped`.
4. The Client aborts the session and the tail of the sentence is lost.

**The offset is the whole argument for the fix.** `diverge_offset=0` means the *first word* was
revised, so no non-zero mark was ever safe. That is why `stability_n/m/hold_ms` cannot help:
delaying the mark only moves when the abort happens. The only sound rule for a backend that
re-decodes from scratch is that nothing is stable until the `final`, which is what
`SessionConfig.publish_stable_partials=False` expresses and what `connection.ConnectionSession`
now derives from the backend's declared `pseudo-streaming` mode.

`TestRevisingBackendAborts` is the regression: it declares the same backend `native-streaming` and
asserts the abort still happens, so the fix cannot be mistaken for the bug having gone away on its
own.
"""

from __future__ import annotations

import unittest

from kvit_asr.backends.base import (
    Backend,
    BackendCapabilities,
    Hypothesis,
    PseudoStreamingBackend,
    ResetReason,
    StreamingMode,
)
from kvit_asr.config import BackendConfig, ServerConfig, SessionConfig as CliSessionConfig
from kvit_asr.connection import LoadedBackend

from tests.test_server import ServerHarness, ServerTestCase, speech, wire_code


# The sequence a growing-window model actually produces. The last entry is the one that bites: it
# re-cases `so` -> `So` at byte 0 and inserts a comma, revising text the first three entries had
# already agreed on. Nothing here is hostile — every entry is a legitimate cumulative hypothesis
# under §8.1, which is the point.
REVISING_SCRIPT = [
    "so I was thinking we",
    "so I was thinking we could change the",
    "so I was thinking we could change the config and",
    "So I was thinking, we could change the config and restart it.",
]


class ScriptedRevisingBackend(Backend):
    """A batch model behind a streaming interface, reduced to the one behaviour that matters.

    One hypothesis per `feed`, cumulative, working through `script` and then holding the last
    entry. `mode` is a constructor argument because the regression test needs the identical
    revision behaviour under a different declaration.
    """

    def __init__(self, script=REVISING_SCRIPT, mode=StreamingMode.PSEUDO_STREAMING) -> None:
        super().__init__()
        self._script = list(script)
        self._mode = mode
        self._i = 0

    @property
    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            model_id="scripted-revising",
            mode=self._mode,
            natural_chunk_ms=0,
            requires_contiguous_audio=False,
            notes="test double for a growing-window batch model",
        )

    def _load(self, progress=None) -> None:
        pass

    def _reset(self, reason: ResetReason) -> None:
        self._i = 0

    def feed(self, pcm: bytes) -> list[Hypothesis]:
        text = self._script[min(self._i, len(self._script) - 1)]
        self._i += 1
        return [Hypothesis(text=text)]

    def finalize(self):
        return Hypothesis(text=self._script[-1], is_final=True, audio_pos_ms=1000)


def revising_loader(mode=StreamingMode.PSEUDO_STREAMING, script=REVISING_SCRIPT):
    """A `backend_loader` for `Server` wrapping the scripted backend in the real facade.

    The real `LoadedBackend` on purpose: the mode this whole test turns on is read off *that*
    object by `ConnectionSession._start`, so a hand-rolled stand-in would test the session while
    leaving the seam that carries the decision unexercised.
    """

    def loader(backend_cfg, on_progress=None, log=None, allow_hostile=False):
        backend = ScriptedRevisingBackend(script=script, mode=mode)
        backend.load()
        return LoadedBackend(backend, log=log, allow_hostile=allow_hostile)

    return loader


CFG = ServerConfig(
    pipe_name=r"\\.\pipe\kvit-dictate-inmemory",
    backend=BackendConfig(id="scripted", model="scripted-revising"),
    session=CliSessionConfig(vad="none"),
)


class _Base(ServerTestCase):
    def run_utterance(self, mode):
        """Drive one utterance to `stopped` and return every message the server sent."""
        h = self.start(backend_loader=revising_loader(mode=mode), cfg=CFG)
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "s1", "sample_rate": 16000})
        for _ in REVISING_SCRIPT:
            h.client.send_audio(speech())
        h.client.send({"t": "stop", "session_id": "s1"})
        # Closing the write side lets `drain` finish on EOF rather than a timeout per message.
        h.client.close_write()
        return h.client.drain()


class TestRevisingBackendIsSafeWhenDeclaredPseudoStreaming(_Base):
    """The fix: a `pseudo-streaming` backend publishes no mark, so there is nothing to contradict."""

    def test_no_error_frame(self):
        msgs = self.run_utterance(StreamingMode.PSEUDO_STREAMING)
        errors = [m for m in msgs if m["t"] == "error"]
        self.assertEqual(
            errors, [], f"a legitimate revision aborted the session: {errors}"
        )

    def test_every_partial_publishes_a_zero_mark(self):
        msgs = self.run_utterance(StreamingMode.PSEUDO_STREAMING)
        partials = [m for m in msgs if m["t"] == "partial"]
        self.assertTrue(partials, "no partials: the live preview would be dead, not merely late")
        for m in partials:
            self.assertEqual(
                m["stable_bytes"], 0,
                "a non-zero mark from a backend that re-decodes from scratch is exactly the bug",
            )

    def test_the_preview_still_revises(self):
        """The user watches the words arrive; only the *commit* waits for the endpoint."""
        msgs = self.run_utterance(StreamingMode.PSEUDO_STREAMING)
        texts = [m["text"] for m in msgs if m["t"] == "partial"]
        self.assertGreater(len(set(texts)), 1, "partials never changed; this is not a live preview")

    def test_the_final_carries_the_whole_utterance(self):
        msgs = self.run_utterance(StreamingMode.PSEUDO_STREAMING)
        finals = [m for m in msgs if m["t"] == "final"]
        self.assertEqual(len(finals), 1)
        self.assertEqual(finals[0]["text"], REVISING_SCRIPT[-1])
        # §6.3: `final.stable_bytes`, when present, MUST equal len(utf8(text)). Nothing was
        # published early, so the whole sentence becomes stable exactly here.
        self.assertEqual(
            finals[0]["stable_bytes"], len(REVISING_SCRIPT[-1].encode("utf-8"))
        )


class TestRevisingBackendAborts(_Base):
    """The regression. Identical backend, declared `native-streaming`: the abort still happens.

    Without this, `publish_stable_partials` could be deleted and the suite above would keep
    passing for the wrong reason — a script that stopped revising, a mark that never advanced.
    """

    def test_declared_native_streaming_still_aborts(self):
        msgs = self.run_utterance(StreamingMode.NATIVE_STREAMING)
        errors = [m for m in msgs if m["t"] == "error"]
        self.assertEqual(len(errors), 1, "the revision no longer contradicts a published mark")
        self.assertEqual(wire_code(errors[0]), "decode_failed")
        self.assertIs(errors[0]["fatal"], False)   # §11.1: the session dies, the connection lives


if __name__ == "__main__":
    unittest.main()


# ─── The other hazard of the same backend class: the window slides in silence ────────────────


class SlidingBackend(PseudoStreamingBackend):
    """A real `PseudoStreamingBackend` with a tiny window, so a slide is cheap to provoke.

    `_transcribe_batch` names each 100 ms of audio by its **absolute** position in the session, so
    the text genuinely tracks the window: as audio scrolls out, leading words disappear from the
    hypothesis and the wrapper's overlap stitch has something real to find. A fixed synthetic
    string would slide the buffer without ever changing the text, and the frozen prefix — the
    thing under test — would never form.
    """

    SLOT_BYTES = 16000 * 2 // 10   # 100 ms of 16 kHz mono s16le

    def __init__(self, window_ms=1000, hop_ms=500):
        super().__init__(window_ms=window_ms, hop_ms=hop_ms)

    @property
    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            model_id="sliding", mode=StreamingMode.PSEUDO_STREAMING, natural_chunk_ms=0
        )

    def _load(self, progress=None) -> None:
        pass

    def _transcribe_batch(self, pcm: bytes) -> str:
        end = self._audio_ms // 100
        start = max(0, end - len(pcm) // self.SLOT_BYTES)
        return " ".join(f"w{i}" for i in range(start, end))


class TestWindowSlideIsReported(unittest.TestCase):
    """Speech outrunning the re-transcription window must not be silent.

    Past `window_ms` the wrapper freezes the text whose audio has gone and stitches the rest on by
    word overlap. Its own docstring says that stitch produces "no exact overlap at all" for a model
    that recases or repunctuates across the boundary — Qwen, per this module's header — and that
    the frozen prefix is "not stable in the §1.4 sense ... merely unrecoverable". So it is a
    quality cliff the stability machinery cannot protect the document from, and it used to be
    crossed with nothing written anywhere.

    This matters more since `--endpoint-silence-ms` went to 1500: fewer splits means more speech
    per utterance, which means closer to the ceiling.
    """

    def setUp(self):
        self.events = []
        events = self.events

        class _Log:
            def warning(self, ev, **f): events.append((ev, f))
            def info(self, ev, **f): pass
            def debug(self, ev, **f): pass
            def error(self, ev, **f): pass

        backend = SlidingBackend()
        backend.load()
        self.facade = LoadedBackend(backend, log=_Log())
        self.facade.begin_session()

    def feed_hops(self, n):
        """One hop of audio per call, so the window fills and then slides the way a session does."""
        for _ in range(n):
            self.facade.accept_audio(speech(500))

    def slides(self):
        return [f for ev, f in self.events if ev == "window_slide"]

    def test_no_warning_while_inside_the_window(self):
        self.feed_hops(2)   # 1000 ms fed into a 1000 ms window: full, nothing gone yet
        self.assertEqual(self.slides(), [], "warned before any audio had scrolled out")

    def test_a_slide_is_warned_about(self):
        self.feed_hops(6)
        slides = self.slides()
        self.assertEqual(len(slides), 1, f"expected exactly one warning, got {self.events}")
        self.assertEqual(slides[0]["window_ms"], 1000)
        self.assertGreater(slides[0]["frozen_bytes"], 0)

    def test_the_warning_carries_no_recognised_text(self):
        """plan.md §1.9: the size of the frozen prefix, never the words in it."""
        self.feed_hops(6)
        blob = repr(self.slides()[0])
        self.assertNotIn("w0", blob)
        self.assertNotIn("w1", blob)

    def test_it_warns_once_per_utterance_not_once_per_hop(self):
        self.feed_hops(12)
        self.assertEqual(len(self.slides()), 1, "a line per hop would bury the one that mattered")

    def test_a_new_utterance_can_warn_again(self):
        self.feed_hops(6)
        self.facade.reset()          # utterance boundary: fresh window, fresh warning
        self.feed_hops(6)
        self.assertEqual(len(self.slides()), 2)
