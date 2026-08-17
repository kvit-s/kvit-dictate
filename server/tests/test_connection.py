r"""Tests for :mod:`kvit_asr.connection` — the three seams between the finished modules.

Run with::

    C:/Python312/python.exe -m unittest discover -s server/tests -t server

``test_server.py`` covers the same code from the outside, over a transport, and that is where the
protocol conversation is asserted. This file is about the translations themselves, because each
one is a place where both sides can be individually correct and the pair still wrong — which is
exactly how the four modules came to disagree in the first place.

The VAD tests lead with quiet audio on purpose. ``EnergyZcrScorer`` calibrates its noise floor
from the quietest frame of the opening ``energy_warmup_ms``, so a stream that opens *on* speech
calibrates the floor to the speech and then hears nothing — a wart its own docstring owns, and one
worth stating in a test rather than discovering in a session.
"""

import array
import os
import sys
import unittest

_SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _SERVER_DIR not in sys.path:
    sys.path.insert(0, _SERVER_DIR)

from kvit_asr import vad as vad_mod  # noqa: E402
from kvit_asr.backends.base import BackendCapabilities, Hypothesis, ResetReason  # noqa: E402
from kvit_asr.backends.base import StreamingMode  # noqa: E402
from kvit_asr.config import BackendConfig, ServerConfig  # noqa: E402
from kvit_asr.config import SessionConfig as CliSessionConfig  # noqa: E402
from kvit_asr.connection import (  # noqa: E402
    ConnectionSession,
    LoadedBackend,
    VadAdapter,
    make_vad,
    open_backend,
)
from kvit_asr.protocol import ProtocolError  # noqa: E402
from kvit_asr.session import SPEECH_END, SPEECH_START  # noqa: E402

SAMPLE_RATE = 16000


def pcm(ms: int, amp: int) -> bytes:
    """16 kHz mono s16le. ``amp`` 5000 is roughly -16 dBFS; 20 is below the VAD's absolute gate."""
    n = SAMPLE_RATE * ms // 1000
    return array.array("h", [amp if i % 2 else -amp for i in range(n)]).tobytes()


def quiet(ms: int) -> bytes:
    return pcm(ms, 20)


def loud(ms: int) -> bytes:
    return pcm(ms, 5000)


# ─── Seam 2: LoadedBackend ──────────────────────────────────────────────────────────────────


class FakeBackend:
    """A `backends.base.Backend` reduced to what `LoadedBackend` touches, recording every call."""

    def __init__(self, *, supports_context: bool = True, chunk_ms: int = 160) -> None:
        self.capabilities = BackendCapabilities(
            model_id="fake-0.1",
            mode=StreamingMode.NATIVE_STREAMING,
            supports_context=supports_context,
            natural_chunk_ms=chunk_ms,
        )
        self.resets: list[ResetReason] = []
        self.contexts: list[list[str]] = []
        self.fed: list[bytes] = []
        self.unloaded = False
        self.hypotheses: list[Hypothesis] = []
        self.final: Hypothesis | None = Hypothesis(text="the whole utterance", is_final=True)

    def reset(self, reason) -> None:
        self.resets.append(reason)

    def set_context(self, terms) -> bool:
        self.contexts.append(list(terms))
        return True

    def feed(self, chunk: bytes):
        self.fed.append(chunk)
        return list(self.hypotheses)

    def finalize(self):
        return self.final

    def unload(self) -> None:
        self.unloaded = True


class TestLoadedBackend(unittest.TestCase):
    def test_the_declaration_fields_come_from_capabilities(self):
        """`ready` is built from these four (docs/protocol.md §6.2), and `mode` in particular must
        be the string of the taxonomy, not the repr of an enum member."""
        b = LoadedBackend(FakeBackend())
        self.assertEqual(b.model_id, "fake-0.1")
        self.assertEqual(b.mode, "native-streaming")
        self.assertEqual(b.chunk_ms, 160)
        self.assertTrue(b.context_supported)

    def test_a_backend_with_no_natural_chunk_reports_none(self):
        """§6.2 makes `chunk_ms` optional, and 0 is not "no chunk" — it is a number the Client
        would have to interpret."""
        self.assertIsNone(LoadedBackend(FakeBackend(chunk_ms=0)).chunk_ms)

    def test_accept_audio_returns_the_newest_hypothesis_never_a_join(self):
        """§8.1: each hypothesis supersedes the previous one for the same utterance. Joining them
        would turn cumulative text into a pile of deltas — the mistake in this area that produces
        no error anywhere."""
        fake = FakeBackend()
        b = LoadedBackend(fake)
        fake.hypotheses = [Hypothesis(text="hello"), Hypothesis(text="hello world")]
        self.assertEqual(b.accept_audio(loud(20)), "hello world")

    def test_accept_audio_returns_none_when_the_backend_said_nothing(self):
        """Zero hypotheses for a chunk is normal and common (base.Backend): an 80 ms model fed a
        20 ms buffer has nothing to say yet. None means "unchanged", not "empty"."""
        self.assertIsNone(LoadedBackend(FakeBackend()).accept_audio(loud(20)))

    def test_finalise_of_an_empty_utterance_is_empty_text(self):
        fake = FakeBackend()
        fake.final = None
        self.assertEqual(LoadedBackend(fake).finalise(), "")

    def test_reset_reasons_survive_the_seam(self):
        """`session.Session` calls `reset(context)` with no reason. base.py: DISCONTINUITY is "the
        one with teeth" — a backend that treats it as an utterance boundary is not wrong, but one
        that never hears about it cannot choose."""
        fake = FakeBackend()
        b = LoadedBackend(fake)
        b.begin_session(["llama.cpp"])
        b.reset(["llama.cpp"])
        b.note_discontinuity()
        b.reset(["llama.cpp"])
        b.reset(["llama.cpp"])
        b.end_session()
        b.end_session(cancelled=True)
        self.assertEqual(
            fake.resets,
            [
                ResetReason.SESSION_START,
                ResetReason.UTTERANCE_BOUNDARY,
                ResetReason.DISCONTINUITY,
                ResetReason.UTTERANCE_BOUNDARY,  # armed once, consumed once
                ResetReason.SESSION_END,
                ResetReason.CANCEL,
            ],
        )

    def test_context_reaches_a_backend_that_supports_it_once_per_change(self):
        fake = FakeBackend()
        b = LoadedBackend(fake)
        b.begin_session(["llama.cpp", "Qwen3"])
        b.reset(["llama.cpp", "Qwen3"])
        b.reset(["llama.cpp", "Qwen3"])
        self.assertEqual(fake.contexts, [["llama.cpp", "Qwen3"]])

    def test_context_is_ignored_by_a_backend_that_declared_no_support(self):
        """§6.1: such a Server MUST ignore `start.context` and MUST report
        `context_supported:false`, which `ready` already did. Ignoring it is conforming; doing it
        silently is what plan.md §4 cannot tolerate, so it is logged once."""

        class Log:
            def __init__(self):
                self.events = []

            def info(self, event, **fields):
                self.events.append((event, fields))

        log = Log()
        fake = FakeBackend(supports_context=False)
        b = LoadedBackend(fake, log=log)
        b.begin_session(["llama.cpp"])
        b.reset(["llama.cpp"])
        self.assertEqual(fake.contexts, [])
        self.assertEqual([e for e, _ in log.events], ["context_ignored"])

    def test_close_unloads(self):
        fake = FakeBackend()
        LoadedBackend(fake).close()
        self.assertTrue(fake.unloaded)


class TestOpenBackend(unittest.TestCase):
    def test_the_mock_loads_from_a_backend_config(self):
        """The default `backend_loader`, on the one backend that is registered and needs nothing
        installed."""
        b = open_backend(BackendConfig(id="mock", model="mock"))
        self.assertEqual(b.mode, "native-streaming")
        self.assertTrue(b.model_id)

    def test_backend_opts_reach_the_constructor_typed(self):
        """`--backend-opt seed=99` arrives as a string; `MockConfig.seed` is an int, and a string
        there would silently change the script rather than fail."""
        b = open_backend(BackendConfig(id="mock", options={"seed": "99"}))
        self.assertEqual(b._backend._cfg.seed, 99)

    def test_an_unknown_backend_is_a_legible_error(self):
        with self.assertRaises(Exception) as ctx:
            open_backend(BackendConfig(id="no-such-backend"))
        self.assertIn("no-such-backend", str(ctx.exception))

    def test_progress_is_reported_to_the_caller(self):
        stages = []
        open_backend(BackendConfig(id="mock"), on_progress=lambda *a: stages.append(a))
        self.assertTrue(stages, "the loader must drive `loading` messages (§6.2)")


# ─── Seam 3: VadAdapter ─────────────────────────────────────────────────────────────────────


class FakeEndpointer:
    """`VadEndpointer`'s surface, so event kinds and positions can be chosen exactly."""

    def __init__(self, position_samples: int = 0) -> None:
        self.position_samples = position_samples
        self.chunks: list[vad_mod.VadChunk] = []
        self.resets = 0
        self.fed = 0

    def process(self, data: bytes):
        self.fed += len(data) // 2
        return self.chunks.pop(0) if self.chunks else vad_mod.VadChunk((), (), False)

    def reset(self) -> None:
        self.resets += 1


def ev(kind, sample):
    return vad_mod.VadEvent(
        kind=kind, sample=sample, decided_at_sample=sample, utterance_index=1,
        sample_rate=SAMPLE_RATE,
    )


class TestVadAdapter(unittest.TestCase):
    def test_an_onset_becomes_a_speech_start_at_a_byte_offset(self):
        e = FakeEndpointer()
        e.chunks = [vad_mod.VadChunk((ev(vad_mod.VadEventKind.SPEECH_START, 100),), (), True)]
        events = VadAdapter(e).push(pcm(50, 5000))
        self.assertEqual([(x.kind, x.offset) for x in events], [(SPEECH_START, 200)])

    def test_an_endpoint_becomes_the_sessions_speech_end(self):
        """`session.Session` closes the utterance — and emits `final` — on SPEECH_END. The
        endpoint is the event that should do that (plan.md §1.6)."""
        e = FakeEndpointer()
        e.chunks = [vad_mod.VadChunk((ev(vad_mod.VadEventKind.UTTERANCE_END, 80),), (), False)]
        events = VadAdapter(e).push(pcm(50, 5000))
        self.assertEqual([(x.kind, x.offset) for x in events], [(SPEECH_END, 160)])

    def test_a_hangover_is_dropped(self):
        """VadEndpointer emits SPEECH_END at every hangover while the utterance survives. Passing
        it on would finalise a sentence every time the speaker drew breath."""
        e = FakeEndpointer()
        e.chunks = [
            vad_mod.VadChunk(
                (
                    ev(vad_mod.VadEventKind.SPEECH_END, 10),
                    ev(vad_mod.VadEventKind.DISCONTINUITY, 20),
                ),
                (),
                True,
            )
        ]
        self.assertEqual(VadAdapter(e).push(pcm(50, 5000)), [])

    def test_a_backdated_onset_clamps_into_the_chunk(self):
        """`ev.sample` is where the speech *was*, which for an onset is deliberately before the
        frame that detected it and can precede this chunk entirely. session.VadEvent documents
        offset 0 as the acceptable coarse answer; a negative offset would make Session slice
        backwards."""
        e = FakeEndpointer()
        adapter = VadAdapter(e)
        e.chunks = [vad_mod.VadChunk((), (), False)]
        adapter.push(pcm(100, 20))  # advance the adapter's chunk origin
        e.chunks = [vad_mod.VadChunk((ev(vad_mod.VadEventKind.SPEECH_START, 5),), (), True)]
        self.assertEqual([x.offset for x in adapter.push(pcm(50, 5000))], [0])

    def test_offsets_never_go_backwards(self):
        """Session walks the events with a single cursor, so a lower offset after a higher one
        would silently drop or duplicate audio."""
        e = FakeEndpointer()
        e.chunks = [
            vad_mod.VadChunk(
                (
                    ev(vad_mod.VadEventKind.UTTERANCE_END, 300),
                    ev(vad_mod.VadEventKind.SPEECH_START, 100),
                ),
                (),
                True,
            )
        ]
        offsets = [x.offset for x in VadAdapter(e).push(pcm(50, 5000))]
        self.assertEqual(offsets, sorted(offsets))

    def test_reset_re_reads_the_endpointers_clock_rather_than_assuming_zero(self):
        """`VadEndpointer.reset` keeps its position clock. An adapter that restarted its own at 0
        would be a whole session's worth of audio out, and every offset would clamp to the chunk
        edge from then on."""
        e = FakeEndpointer()
        adapter = VadAdapter(e)
        adapter.push(pcm(100, 20))
        e.position_samples = 1600
        adapter.reset()
        e.chunks = [vad_mod.VadChunk((ev(vad_mod.VadEventKind.SPEECH_START, 1700),), (), True)]
        self.assertEqual([x.offset for x in adapter.push(pcm(50, 5000))], [200])

    def test_the_real_endpointer_finds_speech_after_quiet(self):
        """One end-to-end pass over the real energy endpointer, so the adapter is not only ever
        tested against a fake with convenient numbers."""
        adapter = make_vad(CliSessionConfig(vad="energy", endpoint_silence_ms=800))
        self.assertIsNotNone(adapter)
        kinds = []
        for chunk in [quiet(300)] + [loud(100)] * 10 + [quiet(100)] * 15:
            kinds += [e.kind for e in adapter.push(chunk)]
        self.assertEqual(kinds, [SPEECH_START, SPEECH_END])

    def test_vad_none_is_a_choice_the_configuration_can_make(self):
        self.assertIsNone(make_vad(CliSessionConfig(vad="none")))


# ─── Seam 1: ConnectionSession ──────────────────────────────────────────────────────────────


class Recorder:
    """Collects what the router sends, so the assertions are about messages, not mocks."""

    def __init__(self):
        self.sent: list[dict] = []

    def __call__(self, msg: dict) -> None:
        self.sent.append(dict(msg))

    def kinds(self) -> list[str]:
        return [m["t"] for m in self.sent]

    def of(self, t: str) -> list[dict]:
        return [m for m in self.sent if m["t"] == t]


class RouterTestCase(unittest.TestCase):
    def router(self, *, vad: str = "none", backend=None, **cfg_kwargs):
        self.sent = Recorder()
        self.backend = LoadedBackend(backend or FakeBackend())
        cfg = ServerConfig(
            pipe_name=r"\\.\pipe\test",
            backend=BackendConfig(id="mock"),
            session=CliSessionConfig(vad=vad, **cfg_kwargs),
        )
        return ConnectionSession(backend=self.backend, send=self.sent, config=cfg)

    def start(self, r, sid="s1", **extra):
        r.handle_control({"t": "start", "session_id": sid, "sample_rate": 16000, **extra})


class TestSessionLifecycle(RouterTestCase):
    def test_start_then_stop_ends_with_stopped(self):
        r = self.router()
        self.start(r)
        self.assertEqual(r.current_session_id, "s1")
        r.handle_control({"t": "stop", "session_id": "s1"})
        self.assertEqual(self.sent.of("stopped")[-1]["reason"], "stop")
        self.assertIsNone(r.current_session_id)

    def test_start_while_open_is_a_fatal_state_violation(self):
        """§7.2: exactly one session per connection. The pipe is not a multiplexer."""
        r = self.router()
        self.start(r)
        with self.assertRaises(ProtocolError) as ctx:
            self.start(r, sid="s2")
        self.assertEqual(ctx.exception.code_raw, "state_violation")
        self.assertTrue(ctx.exception.fatal)

    def test_stop_for_an_unknown_session_is_answered_not_refused(self):
        """§6.1 idempotence and §10.5: the Client's teardown path runs on crash paths where its
        own state is uncertain, and `stop` before `ready` names no session at all."""
        r = self.router()
        r.handle_control({"t": "stop", "session_id": "s9"})
        self.assertEqual(self.sent.of("stopped"), [
            {"t": "stopped", "session_id": "s9", "reason": "unknown_session"}
        ])

    def test_stop_twice_is_answered_twice(self):
        r = self.router()
        self.start(r)
        r.handle_control({"t": "stop", "session_id": "s1"})
        r.handle_control({"t": "stop", "session_id": "s1"})
        reasons = [m["reason"] for m in self.sent.of("stopped")]
        self.assertEqual(reasons, ["stop", "unknown_session"])

    def test_cancel_stops_the_session_and_tells_the_backend(self):
        r = self.router()
        self.start(r)
        r.handle_control({"t": "cancel", "session_id": "s1"})
        self.assertEqual(self.sent.of("stopped")[-1]["reason"], "cancel")
        self.assertIn(ResetReason.CANCEL, self.backend._backend.resets)

    def test_a_second_session_can_follow_the_first(self):
        r = self.router()
        self.start(r)
        r.handle_control({"t": "stop", "session_id": "s1"})
        self.start(r, sid="s2")
        self.assertEqual(r.current_session_id, "s2")

    def test_close_cancels_an_open_session(self):
        r = self.router()
        self.start(r)
        r.close()
        self.assertEqual(self.sent.of("stopped")[-1]["reason"], "cancel")

    def test_a_non_fatal_peer_error_abandons_the_session_only(self):
        """§11.1: `fatal:false` ends the session, not the connection. The Client sends
        `stability_violation` this way when our own `stable_bytes` broke §8.2."""
        r = self.router()
        self.start(r)
        r.handle_control(
            {"t": "error", "session_id": "s1", "code": "stability_violation",
             "fatal": False, "msg": "committed text was contradicted"}
        )
        self.assertIsNone(r.current_session_id)
        self.assertFalse(r.connection_fatal)


class TestAudioRouting(RouterTestCase):
    def test_audio_with_no_session_is_discarded_and_counted(self):
        """§6.3's Server obligation: losing the race between a `stop` and an in-flight audio frame
        is a Client bug this rule contains rather than escalates. Never buffered for the next
        session — that audio belongs to a conversation that is over."""
        r = self.router()
        r.handle_audio(loud(100))
        self.assertEqual(r.discarded_audio_bytes, len(loud(100)))
        self.assertEqual(self.sent.kinds(), [])

    def test_audio_after_stop_is_discarded_too(self):
        r = self.router()
        self.start(r)
        r.handle_control({"t": "stop", "session_id": "s1"})
        r.handle_audio(loud(100))
        self.assertEqual(r.discarded_audio_bytes, len(loud(100)))

    def test_audio_reaches_the_backend_while_a_session_is_open(self):
        r = self.router()
        self.start(r)
        r.handle_audio(loud(100))
        self.assertTrue(self.backend._backend.fed)

    def test_discontinuity_is_accepted_and_is_not_fatal(self):
        """§6.1: the Server MUST accept it and MUST NOT treat it as fatal. The backend hears about
        it as DISCONTINUITY, which is the whole reason the message exists."""
        r = self.router()
        self.start(r)
        r.handle_audio(loud(100))
        r.handle_control(
            {"t": "discontinuity", "session_id": "s1", "gap_ms": 40, "reason": "wasapi"}
        )
        self.assertIn(ResetReason.DISCONTINUITY, self.backend._backend.resets)
        self.assertEqual(r.current_session_id, "s1")
        self.assertEqual(self.sent.of("error"), [])


class TestGatedSession(RouterTestCase):
    """The router with the real energy VAD and the real mock backend, end to end."""

    def test_speech_after_quiet_produces_an_utterance_that_ends_at_the_endpoint(self):
        from kvit_asr.backends.mock import MockBackend, MockConfig

        r = self.router(vad="energy", endpoint_silence_ms=800,
                        backend=self._loaded_mock(MockBackend, MockConfig))
        self.start(r)
        for chunk in [quiet(300)] + [loud(200)] * 12:
            r.handle_audio(chunk)
        self.assertTrue(self.sent.of("partial"), "speech should reach the decoder")

        for chunk in [quiet(200)] * 10:
            r.handle_audio(chunk)
        finals = self.sent.of("final")
        self.assertTrue(finals, "the endpoint silence must close the utterance (plan.md §1.6)")
        self.assertEqual(finals[0]["utterance_id"], 1)
        self.assertEqual(r.current_session_id, "s1", "the endpoint ends the utterance, not the "
                                                     "session (plan.md §1.6)")

    def _loaded_mock(self, MockBackend, MockConfig):
        mock = MockBackend(MockConfig(seed=3))
        mock.load()
        return mock


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
