"""The VAD gate on `openai-http`: don't ask a transcription model to transcribe silence.

Handed non-speech, the model does not return nothing — it invents. Measured against the live
endpoint on 2026-08-21: 500 ms of digital silence returns ``"Yeah."``, 2000 ms returns ``"Okay."``,
click transients return ``"Hi."``. Under `Cadence::EndOfSession` those land in a session-long
buffer and are typed into the document along with everything real.

The server grew a `/v1/audio/vad` route (Silero via pysilero-vad), so the backend can now ask
before it spends a transcription. What has to hold:

1. **Off by default.** A backend with `vad_gate=False` must make exactly the requests it always
   did — no VAD call, same result.
2. **A "no speech" verdict skips the transcription entirely**, and returns ``""`` rather than
   raising. The pseudo-streaming wrapper reads that as "this window produced no hypothesis",
   which is the truth.
3. **Speech is untouched.** The gate must never alter or shorten a real transcription.
4. **It fails open.** A missing route (503), a timeout, a malformed body — anything — transcribes
   anyway. Losing what someone actually said is worse than an occasional invented "Okay.", and a
   VAD outage must not be able to silence dictation.
5. **It complains once, not once per hop**, and stops asking after the first refusal — a
   two-second hop would otherwise put a warning in the log every two seconds all session.

Offline: `urlopen` is replaced, so nothing here touches the network.
"""

from __future__ import annotations

import io
import json
import unittest
from unittest import mock

from kvit_asr.backends import openai_http as mod
from kvit_asr.backends.openai_http import OpenAiHttpBackend

SILENCE = b"\x00\x00" * 16000       # 1 s at 16 kHz mono s16le


class _Resp(io.BytesIO):
    """Minimal stand-in for the object `urlopen` returns as a context manager."""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _Server:
    """A scripted endpoint. Records every path asked for, so "did not call VAD" is testable."""

    def __init__(self, *, speech=True, vad_error=None, text="hello world"):
        self.speech = speech
        self.vad_error = vad_error
        self.text = text
        self.paths: list[str] = []

    def __call__(self, req, timeout=None):
        url = req.full_url
        self.paths.append(url)
        if mod._VAD_PATH in url:
            if self.vad_error is not None:
                raise self.vad_error
            return _Resp(json.dumps({"speech": self.speech, "segments": []}).encode())
        return _Resp(json.dumps({"text": self.text}).encode())

    @property
    def vad_calls(self) -> int:
        return sum(1 for p in self.paths if mod._VAD_PATH in p)

    @property
    def transcribe_calls(self) -> int:
        return sum(1 for p in self.paths if mod._PATH in p)


def backend(**kw) -> OpenAiHttpBackend:
    b = OpenAiHttpBackend(base_url="http://test.invalid", model="m", **kw)
    b._loaded = True    # `load()` would probe the network; the gate is what is under test
    return b


class TestGateOffByDefault(unittest.TestCase):
    def test_no_vad_request_is_made(self):
        srv = _Server(speech=False)
        b = backend()
        with mock.patch.object(mod.urllib.request, "urlopen", srv):
            self.assertEqual(b._transcribe_batch(SILENCE), "hello world")
        self.assertEqual(srv.vad_calls, 0, "asked the VAD route without being told to")
        self.assertEqual(srv.transcribe_calls, 1)


class TestGateOn(unittest.TestCase):
    def test_no_speech_skips_the_transcription(self):
        srv = _Server(speech=False)
        b = backend(vad_gate=True)
        with mock.patch.object(mod.urllib.request, "urlopen", srv):
            self.assertEqual(b._transcribe_batch(SILENCE), "")
        self.assertEqual(srv.vad_calls, 1)
        self.assertEqual(srv.transcribe_calls, 0, "spent a transcription on silence anyway")
        self.assertEqual(b.vad_skipped_count, 1)

    def test_speech_is_transcribed_unchanged(self):
        srv = _Server(speech=True)
        b = backend(vad_gate=True)
        with mock.patch.object(mod.urllib.request, "urlopen", srv):
            self.assertEqual(b._transcribe_batch(SILENCE), "hello world")
        self.assertEqual(srv.transcribe_calls, 1)
        self.assertEqual(b.vad_skipped_count, 0)

    def test_threshold_and_gain_reach_the_wire_when_set(self):
        """The route takes the bridge's own tuning; a backend that dropped it would look correct
        and quietly use the defaults, which is the failure this repo keeps rediscovering."""
        sent = {}

        def capture(req, timeout=None):
            if mod._VAD_PATH in req.full_url:
                sent["body"] = req.data
                return _Resp(json.dumps({"speech": True}).encode())
            return _Resp(json.dumps({"text": "x"}).encode())

        b = backend(vad_gate=True, vad_threshold=0.7, vad_gain=4.0)
        with mock.patch.object(mod.urllib.request, "urlopen", capture):
            b._transcribe_batch(SILENCE)
        body = sent["body"]
        self.assertIn(b"threshold", body)
        self.assertIn(b"0.7", body)
        self.assertIn(b"gain", body)
        self.assertIn(b"4.0", body)


class TestFailsOpen(unittest.TestCase):
    """Every failure of the detector must end in the audio still being transcribed."""

    def _assert_open(self, err):
        srv = _Server(speech=False, vad_error=err)
        b = backend(vad_gate=True)
        with mock.patch.object(mod.urllib.request, "urlopen", srv):
            self.assertEqual(b._transcribe_batch(SILENCE), "hello world",
                             f"{type(err).__name__} silenced the transcription")
        self.assertEqual(srv.transcribe_calls, 1)

    def test_route_missing(self):
        import urllib.error
        self._assert_open(urllib.error.HTTPError(
            "http://test.invalid/v1/audio/vad", 503, "no pysilero-vad", {}, None))

    def test_timeout(self):
        self._assert_open(TimeoutError("timed out"))

    def test_malformed_body(self):
        self._assert_open(ValueError("not json"))

    def test_it_stops_asking_and_warns_once(self):
        srv = _Server(speech=False, vad_error=TimeoutError("timed out"))
        b = backend(vad_gate=True)
        with mock.patch.object(mod.urllib.request, "urlopen", srv):
            with self.assertLogs(mod.logger, level="WARNING") as caught:
                for _ in range(5):
                    b._transcribe_batch(SILENCE)
        self.assertEqual(len(caught.output), 1, "warned on every hop instead of once")
        self.assertEqual(srv.vad_calls, 1, "kept asking a route already known to be absent")
        self.assertEqual(srv.transcribe_calls, 5)


class TestConfigPlumbing(unittest.TestCase):
    """`--backend-opt` arrives as strings; a flag that stayed a string would be truthy always."""

    def test_from_backend_config_coerces(self):
        cfg = type("C", (), {"model": "m", "chunk_ms": 2000,
                             "options": {"vad_gate": "true", "vad_threshold": "0.6",
                                         "vad_gain": "4", "window_ms": "30000"}})()
        b = OpenAiHttpBackend.from_backend_config(cfg)
        self.assertIs(b._vad_gate, True)
        self.assertEqual(b._vad_threshold, 0.6)
        self.assertEqual(b._vad_gain, 4.0)
        self.assertEqual(b.window_ms, 30000)
        self.assertEqual(b.hop_ms, 2000)

    def test_vad_gate_false_spelling(self):
        cfg = type("C", (), {"model": "m", "chunk_ms": None,
                             "options": {"vad_gate": "false"}})()
        self.assertIs(OpenAiHttpBackend.from_backend_config(cfg)._vad_gate, False)


if __name__ == "__main__":
    unittest.main()
