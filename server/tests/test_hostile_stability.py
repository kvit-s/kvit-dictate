"""`Hypothesis.stable_bytes_override`: the path from a hostile backend to a non-conforming wire.

docs/protocol.md §8.3 says what a Client must do when a Server retracts already-published stable
text. That handling is covered by frozen fixtures on the C++ side, but it had never been reachable
from a running sidecar, for a structural reason: stability is the *server's* computation
(plan.md §1.4), so no correct pipeline can produce §8.3's input. `backends.base.Hypothesis` carries
`stable_bytes_override` as the deliberate hole in that argument, and `backends.mock` has produced
one since it was written — but `connection.LoadedBackend` passed only `text` across the seam, so
the number was manufactured and then dropped one function later. models.json describes an
end-to-end hostile path; until this, it did not exist.

Four things have to hold, and each has a way of failing that leaves the suite looking healthy:

1. **Refused by default.** A hostile mock without `--allow-hostile-backend` must behave exactly
   like a well-behaved one — and say so in the log. A silent refusal is the dangerous outcome: a
   fixture run that proved nothing looks identical to one that passed.
2. **Honoured when asked for, and logged every time.** base.py's field docstring requires both.
3. **The server's own stability computation stands down.** This is the subtle one. The tracker
   raises `StabilityViolation` exactly when a hypothesis contradicts what was published, which is
   what a hostile backend is *for*; left running it aborts the utterance with `decode_failed`
   before the bad frame reaches the wire, and the §8.3 handling stays untested while the test
   appears to run.
4. **Scoped to one utterance.** §8.2's requirements are per `utterance_id`, so the next utterance
   is computed normally even against the same backend.
"""

from __future__ import annotations

import unittest

from kvit_asr.backends.base import Hypothesis
from kvit_asr.config import BackendConfig, ServerConfig, SessionConfig as CliSessionConfig, parse_args
from kvit_asr.connection import LoadedBackend

from tests.test_server import (
    ServerHarness,
    ServerTestCase,
    mock_backend_loader,
    speech,
)

from kvit_asr.backends.mock import HostileMode, MockBackend, MockConfig


# ─── The seam: LoadedBackend ────────────────────────────────────────────────────────────────


class _Recorder:
    """Collects structured log calls the way `EventLogger` is called."""

    def __init__(self) -> None:
        self.events: list[tuple[str, str, dict]] = []

    def _record(self, level):
        def call(event, **fields):
            self.events.append((level, event, fields))

        return call

    def __getattr__(self, name):
        if name in ("debug", "info", "warning", "error"):
            return self._record(name)
        raise AttributeError(name)

    def of(self, event: str) -> list[dict]:
        return [f for _lvl, e, f in self.events if e == event]


class _Offering:
    """A backend that offers an override on every hypothesis it produces."""

    def __init__(self, override: int | None = 4) -> None:
        self.override = override
        self.capabilities = type(
            "Caps", (), {"model_id": "offering", "mode": "native-streaming",
                         "natural_chunk_ms": 0, "supports_context": False}
        )()

    def feed(self, pcm):
        return [Hypothesis(text="hello world", stable_bytes_override=self.override, hostile=True)]

    def finalize(self):
        return Hypothesis(
            text="hello world", is_final=True,
            stable_bytes_override=self.override, hostile=True,
        )

    def reset(self, reason=None):
        pass

    def unload(self):
        pass

    def set_context(self, terms):
        pass


class SeamTests(unittest.TestCase):
    def test_refused_without_the_flag(self) -> None:
        log = _Recorder()
        facade = LoadedBackend(_Offering(), log=log, allow_hostile=False)
        facade.accept_audio(b"\x00\x00")
        self.assertIsNone(facade.pending_stable_bytes_override())

    def test_refusal_is_logged_once_and_names_the_flag(self) -> None:
        """Loud enough to notice, quiet enough not to drown the run: once per connection."""
        log = _Recorder()
        facade = LoadedBackend(_Offering(), log=log, allow_hostile=False)
        for _ in range(5):
            facade.accept_audio(b"\x00\x00")
        refusals = log.of("hostile_override_refused")
        self.assertEqual(len(refusals), 1)
        self.assertIn("--allow-hostile-backend", refusals[0]["detail"])

    def test_honoured_with_the_flag(self) -> None:
        facade = LoadedBackend(_Offering(7), log=_Recorder(), allow_hostile=True)
        facade.accept_audio(b"\x00\x00")
        self.assertEqual(facade.pending_stable_bytes_override(), 7)

    def test_every_honoured_override_is_logged(self) -> None:
        """Each one is a deliberately non-conforming frame; the log is the only record of which."""
        log = _Recorder()
        facade = LoadedBackend(_Offering(7), log=log, allow_hostile=True)
        for _ in range(3):
            facade.accept_audio(b"\x00\x00")
        honoured = log.of("hostile_override_honoured")
        self.assertEqual(len(honoured), 3)
        self.assertEqual(honoured[0]["stable_bytes"], 7)
        self.assertTrue(honoured[0]["hostile"])
        self.assertEqual([lvl for lvl, e, _ in log.events if e == "hostile_override_honoured"],
                         ["error", "error", "error"])

    def test_a_final_hypothesis_offers_one_too(self) -> None:
        """The seam reports it; `Session` deliberately declines to publish it on a `final`.

        See `Session._close_utterance`: `final.stable_bytes` MUST equal len(utf8(text)) (§6.2, and
        the decoder enforces it), so a forced partial mark there would be rejected as
        `bad_message` — the wrong reason — instead of provoking §8.3.
        """
        facade = LoadedBackend(_Offering(3), log=_Recorder(), allow_hostile=True)
        self.assertEqual(facade.finalise(), "hello world")
        self.assertEqual(facade.pending_stable_bytes_override(), 3)

    def test_a_well_behaved_backend_offers_nothing(self) -> None:
        facade = LoadedBackend(_Offering(None), log=_Recorder(), allow_hostile=True)
        facade.accept_audio(b"\x00\x00")
        self.assertIsNone(facade.pending_stable_bytes_override())

    def test_the_override_does_not_outlive_an_empty_feed(self) -> None:
        """A stale override applied to a later hypothesis would forge a violation nobody asked
        for — and would do it *after* the hostile mode had stopped."""

        class _Once(_Offering):
            def __init__(self):
                super().__init__(5)
                self.n = 0

            def feed(self, pcm):
                self.n += 1
                if self.n == 1:
                    return super().feed(pcm)
                return []

        facade = LoadedBackend(_Once(), log=_Recorder(), allow_hostile=True)
        facade.accept_audio(b"\x00\x00")
        self.assertEqual(facade.pending_stable_bytes_override(), 5)
        self.assertIsNone(facade.accept_audio(b"\x00\x00"))
        self.assertIsNone(facade.pending_stable_bytes_override())


# ─── The flag ───────────────────────────────────────────────────────────────────────────────


class FlagTests(unittest.TestCase):
    BASE = ["--pipe", r"\\.\pipe\kvit-test", "--backend", "mock"]

    def test_defaults_to_off(self) -> None:
        self.assertFalse(parse_args(self.BASE).allow_hostile_backend)

    def test_turns_on(self) -> None:
        self.assertTrue(
            parse_args([*self.BASE, "--allow-hostile-backend"]).allow_hostile_backend
        )

    def test_is_not_reachable_through_backend_opt(self) -> None:
        """A backend must not be able to authorise its own protocol violations.

        `--backend-opt` values are read by the backend and supplied alongside it, so routing the
        permission through them would put the decision on the wrong side of the seam. The flag is
        server-level and stays there; this pins that it does not leak into the backend's options.
        """
        cfg = parse_args([*self.BASE, "--backend-opt", "allow_hostile=true"])
        self.assertFalse(cfg.allow_hostile_backend)
        self.assertEqual(cfg.backend.options, {"allow_hostile": "true"})


class ModelsJsonEntryTests(unittest.TestCase):
    """The `mock-hostile` entry, driven exactly as models.json spells it.

    Parsing the args is not enough — `test_backends_registry` already does that. This builds the
    backend the parsed args describe, because the last link in the chain was broken in a way only
    construction could show: `--backend-opt` delivers strings, and `hostile_stability=decrease`
    survived as a `str` all the way to `capabilities`, where it failed with an `AttributeError`
    about `str` that said nothing about the command line that caused it.
    """

    @staticmethod
    def _entry_args() -> list[str]:
        import json
        from pathlib import Path

        manifest = json.loads(
            (Path(__file__).resolve().parents[2] / "models.json").read_text(encoding="utf-8")
        )
        entry = next(e for e in manifest["models"] if e["id"] == "mock-hostile")
        return list(entry["args"])

    def test_the_entry_builds_a_backend_that_actually_lies(self) -> None:
        from kvit_asr.connection import open_backend

        cfg = parse_args(["--pipe", r"\\.\pipe\kvit-test", *self._entry_args()])
        self.assertTrue(cfg.allow_hostile_backend)

        log = _Recorder()
        facade = open_backend(cfg.backend, log=log, allow_hostile=cfg.allow_hostile_backend)
        for _ in range(40):
            facade.accept_audio(b"\x01\x00" * 1600)
            if facade.pending_stable_bytes_override() is not None:
                break
        else:
            self.fail("the mock-hostile entry never produced an override")
        self.assertTrue(log.of("hostile_override_honoured"))

    def test_string_enums_from_the_command_line_are_coerced(self) -> None:
        backend = MockBackend(hostile_stability="mutate", silence_behaviour="hallucinate")
        self.assertIs(backend._cfg.hostile_stability, HostileMode.MUTATE)
        self.assertIn("HOSTILE(mutate)", backend.capabilities.notes)

    def test_an_unknown_mode_names_the_ones_that_exist(self) -> None:
        with self.assertRaises(ValueError) as caught:
            MockBackend(hostile_stability="sideways")
        self.assertIn("sideways", str(caught.exception))
        self.assertIn("decrease", str(caught.exception))


# ─── End to end, on the wire ────────────────────────────────────────────────────────────────


def _hostile_cfg(allow: bool) -> ServerConfig:
    return ServerConfig(
        pipe_name=r"\\.\pipe\kvit-dictate-inmemory",
        backend=BackendConfig(id="mock", model="mock"),
        session=CliSessionConfig(vad="none"),
        allow_hostile_backend=allow,
    )


class WireTests(ServerTestCase):
    """The whole point: a decreasing `stable_bytes` actually reaching a Client."""

    def _session(self, mode: HostileMode, allow: bool, chunks: int = 24) -> list[dict]:
        """Every message the Client saw. Not `expect`, which swallows the text stream."""
        h = self.start(
            cfg=_hostile_cfg(allow),
            backend_loader=mock_backend_loader(
                MockConfig(seed=17, hostile_stability=mode, hostile_after_emissions=2)
            ),
        )
        self.harness_ref = h
        self.ready(h.client)
        h.client.send({"t": "start", "session_id": "h1", "sample_rate": 16000})
        for _ in range(chunks):
            h.client.send_audio(speech())
        h.client.send({"t": "stop", "session_id": "h1"})
        # Closing the write side lets `drain` finish on EOF instead of on a timeout per message.
        h.client.close_write()
        msgs = h.client.drain()
        self.assertIn("stopped", [m["t"] for m in msgs])
        return msgs

    def _run(self, mode: HostileMode, allow: bool, chunks: int = 24) -> list[dict]:
        return [
            m for m in self._session(mode, allow, chunks) if m.get("t") in ("partial", "final")
        ]

    @staticmethod
    def _marks_by_utterance(texts: list[dict]) -> dict[int, list[int]]:
        out: dict[int, list[int]] = {}
        for m in texts:
            out.setdefault(m["utterance_id"], []).append(m["stable_bytes"])
        return out

    def test_decrease_reaches_the_wire_when_allowed(self) -> None:
        """§8.2 requirement 3, first half: `stable_bytes` must never decrease within an utterance.

        Here it does — deliberately — because that is the input the Client's §8.3 handling exists
        to reject and nothing else can produce it.
        """
        by_utterance = self._marks_by_utterance(self._run(HostileMode.DECREASE, allow=True))
        decreased = any(
            any(b < a for a, b in zip(marks, marks[1:])) for marks in by_utterance.values()
        )
        self.assertTrue(
            decreased,
            f"expected a decreasing stable_bytes somewhere in {by_utterance}",
        )

    def test_the_session_survives_it(self) -> None:
        """Emitting a violation is not the same as failing. The sidecar knows what it is doing;
        it must not abort the utterance with `decode_failed` on its own output."""
        msgs = self._session(HostileMode.DECREASE, allow=True)
        kinds = [m["t"] for m in msgs]
        self.assertNotIn("error", kinds)
        stopped = next(m for m in msgs if m["t"] == "stopped")
        self.assertEqual(stopped["reason"], "stop")

    def test_a_final_still_conforms(self) -> None:
        """The override shapes `partial` only. §6.2: `final.stable_bytes` is the whole text."""
        finals = [m for m in self._run(HostileMode.DECREASE, allow=True) if m["t"] == "final"]
        self.assertTrue(finals)
        for m in finals:
            with self.subTest(utterance_id=m["utterance_id"]):
                self.assertEqual(m["stable_bytes"], len(m["text"].encode("utf-8")))

    def test_nothing_illegal_reaches_the_wire_without_the_flag(self) -> None:
        """The same hostile mock, unauthorised: `stable_bytes` is monotonic per utterance again."""
        by_utterance = self._marks_by_utterance(self._run(HostileMode.DECREASE, allow=False))
        self.assertTrue(by_utterance)
        for uid, marks in by_utterance.items():
            with self.subTest(utterance_id=uid):
                self.assertEqual(marks, sorted(marks), f"stable_bytes went backwards: {marks}")

    def test_mutate_reaches_the_wire_when_allowed(self) -> None:
        """§8.2 requirement 3, second half — the one with teeth.

        `stable_bytes` never decreases, but the bytes *below* it change. A receiver comparing only
        the numbers sees nothing wrong while text it may already have typed has been retracted.
        """
        texts = self._run(HostileMode.MUTATE, allow=True)
        published: dict[int, tuple[bytes, int]] = {}
        mutated = False
        for m in texts:
            uid, mark = m["utterance_id"], m["stable_bytes"]
            prefix = m["text"].encode("utf-8")[:mark]
            prev = published.get(uid)
            if prev is not None:
                prev_prefix, prev_mark = prev
                shared = min(prev_mark, mark)
                if prefix[:shared] != prev_prefix[:shared]:
                    mutated = True
            published[uid] = (prefix, mark)
        self.assertTrue(mutated, "no published stable prefix ever changed underneath its offset")

    def test_the_refusal_is_visible_in_the_log(self) -> None:
        self._session(HostileMode.DECREASE, allow=False)
        self.assertTrue(
            any("hostile_override_refused" in ln for ln in self.harness_ref.log_lines),
            "a hostile backend that was refused must not be silent about it",
        )

    def test_honouring_is_visible_in_the_log(self) -> None:
        self._session(HostileMode.DECREASE, allow=True)
        self.assertTrue(
            any("hostile_override_honoured" in ln for ln in self.harness_ref.log_lines),
            "every manufactured frame must be recorded; the log is the only record of which",
        )


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
