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
