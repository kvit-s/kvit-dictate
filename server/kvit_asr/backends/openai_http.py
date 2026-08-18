"""Transcription over an OpenAI-compatible ``/v1/audio/transcriptions`` endpoint.

Written for a specific box: a Qwen3-ASR server on the operator's own LAN
(``http://strixhalo.lan:10301``). It is the fastest route from "the client works against a
scripted mock" to "the client works against a real ASR model", because unlike every other backend
here it needs **no environment, no download and no third-party package** — `urllib` and `wave` are
standard library, so this module loads under a bare CPython, including the empty `mock` env.

──────────────────────────────────────────────────────────────────────────────────────────────
This backend talks to the network, and plan.md says not to
──────────────────────────────────────────────────────────────────────────────────────────────

plan.md §2 lists **"No network access during dictation. No telemetry, no cloud fallback"** among
the product's guarantees, and this module breaks the letter of it on every hop. Recorded here
rather than quietly, because a reader who finds an HTTP client in a local-first dictation tool
deserves to know it was deliberate:

* the endpoint is a machine on the operator's own network, not a cloud service, so the *privacy*
  intent of that rule — audio never leaves hardware the user controls — is intact;
* the *availability* intent is not. A LAN round trip per hop means dictation now depends on
  another host being up, and this backend fails the "works on a plane" test that a local model
  passes trivially.

So this is a development and bring-up backend. It is not a candidate in bench/CANDIDATES.md §4
and must not be reported in the plan.md §4 bake-off: its latency is a property of somebody's
network and GPU queue, not of an ASR architecture.

──────────────────────────────────────────────────────────────────────────────────────────────
Pseudo-streaming, and why that is a worse fit here than for Whisper
──────────────────────────────────────────────────────────────────────────────────────────────

The endpoint is batch: you post a recording, you get a string. So this is a
:class:`PseudoStreamingBackend` — an overlapping window re-transcribed from scratch every hop —
and it declares ``StreamingMode.PSEUDO_STREAMING`` like the Whisper baseline does.

The cost is worse than Whisper's, though, and the difference is worth stating. A local batch
model re-decodes in-process; here every hop is an HTTP request carrying the whole window as a WAV.
An 8-second window at the default 1000 ms hop is eight requests, each uploading up to 256 KB, each
waiting on a shared serialisation lock on the far end. `hop_ms` therefore defaults to **1000 ms
rather than the base class's 640**, and the honest guidance is to raise it further, or set
``finalize_only=True`` and accept no live preview at all, if the server is busy.

``feed()`` is synchronous by the base class's contract, so a slow response blocks the sidecar's
audio path for its duration. Audio is not lost — the caller buffers — but latency accumulates.
That is a real limitation of running a remote batch model behind a streaming interface and it is
not worked around here.

──────────────────────────────────────────────────────────────────────────────────────────────
It hallucinates over silence, measured rather than assumed
──────────────────────────────────────────────────────────────────────────────────────────────

plan.md §1.6 records this for Whisper, quoting the neighbouring project's `transcribe.py` notes.
This endpoint's model does it too, and it was measured here on the first day it was wired up::

    500 ms of pure digital silence  ->  'Yeah.'
    2000 ms of pure digital silence ->  'Okay.'

**No guard is implemented in this module, deliberately.** The Whisper backend can filter on
segment timestamps because `transformers` returns them; this endpoint returns a bare string, so
there is nothing here to filter *on*. A phrase blocklist was rejected for the reason
`whisper_transformers.py` already gives — "thank you" and "okay" are perfectly ordinary dictated
words, and silently deleting text the user said is the failure mode this project is least willing
to accept.

The real fix is upstream and already exists: plan.md §1.6's VAD gating stops feeding at the
endpoint, so silence never reaches a backend during normal operation. This matters for the
`--vad none` entries, and it is why there is no ``mock-ungated`` equivalent for this backend.
"""

from __future__ import annotations

import io
import json
import socket
import urllib.error
import urllib.request
import wave
from typing import Any, Optional

from .base import (
    SAMPLE_RATE,
    CHANNELS,
    BYTES_PER_SAMPLE,
    BackendCapabilities,
    BackendDecodeError,
    BackendLoadError,
    ProgressFn,
    PseudoStreamingBackend,
    StreamingMode,
)

__all__ = ["OpenAiHttpBackend", "DEFAULT_BASE_URL", "DEFAULT_MODEL"]

DEFAULT_BASE_URL = "http://strixhalo.lan:10301"
DEFAULT_MODEL = "Qwen/Qwen3-ASR-1.7B"

# The endpoint accepts anything ffmpeg can decode. WAV is chosen because the standard library can
# write it and because it is lossless: re-encoding to a compressed format on the way out would put
# a codec between the measured audio and the model for no benefit.
_PATH = "/v1/audio/transcriptions"


def _wav_bytes(pcm: bytes) -> bytes:
    """Wrap raw PCM in a WAV container. The protocol fixes the format (docs/protocol.md §6.3), so
    the header is not negotiable and is not parameterised."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(BYTES_PER_SAMPLE)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()


def _multipart(fields: dict, filename: str, payload: bytes) -> tuple[bytes, str]:
    """Encode one file plus some text fields as ``multipart/form-data``.

    Hand-rolled because the standard library has no multipart *encoder* — `email.mime` can be bent
    into one, but it rewrites line endings and folds long headers, and a body that has been
    silently re-wrapped is a bad thing to debug against somebody else's server.

    The boundary is a fixed string rather than a random one. Randomness here would need a source
    (`uuid`, `os.urandom`) and buys nothing: the parts are audio/WAV and short ASCII values, and a
    WAV cannot contain this token because its own body is length-prefixed, not delimited.
    """
    boundary = "----kvit-dictate-boundary-7Nq2Xr"
    out = bytearray()
    for key, value in fields.items():
        if value is None:
            continue
        out += f"--{boundary}\r\n".encode("ascii")
        out += f'Content-Disposition: form-data; name="{key}"\r\n\r\n'.encode("ascii")
        out += str(value).encode("utf-8")
        out += b"\r\n"
    out += f"--{boundary}\r\n".encode("ascii")
    out += (
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        f"Content-Type: audio/wav\r\n\r\n"
    ).encode("ascii")
    out += payload
    out += b"\r\n"
    out += f"--{boundary}--\r\n".encode("ascii")
    return bytes(out), f"multipart/form-data; boundary={boundary}"


class OpenAiHttpBackend(PseudoStreamingBackend):
    """`--backend openai-http`. See the module docstring for what this is and is not for."""

    def __init__(
        self,
        *,
        base_url: str = DEFAULT_BASE_URL,
        model: str = DEFAULT_MODEL,
        language: Optional[str] = "en",
        timeout_s: float = 30.0,
        api_key: Optional[str] = None,
        window_ms: int = 8000,
        hop_ms: int = 1000,
        finalize_only: bool = False,
    ) -> None:
        # finalize_only turns the hop off entirely by making it longer than any window can be, so
        # the only transcription is the one finalize() forces. One request per utterance, no live
        # preview. It is the setting to reach for when the server is shared or slow, and it is the
        # UX plan.md §3 calls "finalize-only".
        if finalize_only:
            hop_ms = max(hop_ms, window_ms)
        super().__init__(window_ms=window_ms, hop_ms=hop_ms)
        self._base_url = base_url.rstrip("/")
        self._model = model
        self._language = language or None
        self._timeout_s = float(timeout_s)
        self._api_key = api_key or None
        self._finalize_only = bool(finalize_only)
        # Counters a result file can carry, so a slow or flaky run is attributable afterwards
        # rather than remembered.
        self.request_count = 0
        self.request_ms_total = 0
        self.empty_response_count = 0

    # -- declaration --------------------------------------------------------------------------

    @property
    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            model_id=self._model,
            mode=StreamingMode.PSEUDO_STREAMING,
            supports_context=False,
            natural_chunk_ms=self.hop_ms,
            chunk_ms_configurable=True,
            # Every request is independent: the window is uploaded whole and the far end keeps no
            # state between calls. A gap in the audio costs content, never decoder coherence, so
            # the server does not need to force a reset around a discontinuity.
            requires_contiguous_audio=False,
            notes=(
                f"HTTP {self._base_url}{_PATH}; model={self._model}; "
                f"language={self._language or 'auto'}; "
                f"{'finalize-only' if self._finalize_only else f'hop={self.hop_ms}ms'}; "
                f"window={self.window_ms}ms. Remote batch model behind a streaming interface - "
                f"latency is a property of the network and the far end's queue, NOT of an ASR "
                f"architecture, and must not be reported in plan.md section 4."
            ),
        )

    # -- construction from the command line ---------------------------------------------------

    @classmethod
    def from_backend_config(cls, cfg: Any) -> "OpenAiHttpBackend":
        """`kvit_asr.connection.open_backend`'s hook. Without it every typed field of the command
        line — `--model`, `--chunk-ms` — is dropped and the defaults above are used silently,
        which is the defect the other backends' hooks were added to fix."""
        options: dict[str, Any] = {}
        if getattr(cfg, "model", None):
            options["model"] = cfg.model
        if getattr(cfg, "chunk_ms", None) is not None:
            options["hop_ms"] = cfg.chunk_ms
        # --backend-opt last, so it overrides anything derived here.
        for key, value in (getattr(cfg, "options", None) or {}).items():
            options[key] = value
        # The flat options arrive as strings from the command line.
        for flag in ("finalize_only",):
            if flag in options and isinstance(options[flag], str):
                options[flag] = options[flag].strip().lower() in ("1", "true", "yes", "on")
        for num in ("hop_ms", "window_ms"):
            if num in options and isinstance(options[num], str):
                options[num] = int(options[num])
        if "timeout_s" in options and isinstance(options["timeout_s"], str):
            options["timeout_s"] = float(options["timeout_s"])
        return cls(**options)

    # -- lifecycle ----------------------------------------------------------------------------

    def _load(self, progress: Optional[ProgressFn]) -> None:
        """There are no weights to load, so "load" means "prove the endpoint is reachable".

        Worth doing rather than skipping: without it the first failure would surface mid-utterance
        as a decode error, with the user already talking. A TCP connect is enough to separate "the
        host is down or the name does not resolve" from "the endpoint rejected the request", and
        it costs no inference on a shared box. The base class's `warm_up()` follows and exercises
        the real POST path with silence, which is the end-to-end check.
        """
        self._report(progress, "connect", 0)
        host, _, port_s = self._base_url.split("//", 1)[1].partition(":")
        port = int(port_s or "80")
        try:
            socket.create_connection((host, port), timeout=min(self._timeout_s, 10.0)).close()
        except OSError as exc:
            raise BackendLoadError(
                f"cannot reach the transcription endpoint at {self._base_url} "
                f"({type(exc).__name__}: {exc}). This backend needs that host to be up; there is "
                f"no local fallback."
            ) from exc
        self._report(progress, "connect", 100)

    # -- the one thing PseudoStreamingBackend asks for -----------------------------------------

    def _transcribe_batch(self, pcm: bytes) -> str:
        if not pcm:
            return ""
        body, content_type = _multipart(
            {
                "model": self._model,
                "language": self._language,
                "response_format": "json",
            },
            "utterance.wav",
            _wav_bytes(pcm),
        )
        req = urllib.request.Request(self._base_url + _PATH, data=body, method="POST")
        req.add_header("Content-Type", content_type)
        req.add_header("Content-Length", str(len(body)))
        if self._api_key:
            req.add_header("Authorization", f"Bearer {self._api_key}")

        import time  # noqa: PLC0415 - only needed on this path

        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(req, timeout=self._timeout_s) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as exc:
            detail = ""
            try:
                detail = exc.read().decode("utf-8", "replace")[:400]
            except Exception:  # noqa: BLE001 - the body is best-effort context, never required
                pass
            raise BackendDecodeError(
                f"transcription endpoint returned HTTP {exc.code}: {detail or exc.reason}"
            ) from exc
        except (urllib.error.URLError, OSError) as exc:
            raise BackendDecodeError(
                f"transcription request to {self._base_url} failed "
                f"({type(exc).__name__}: {exc})"
            ) from exc
        finally:
            self.request_count += 1
            self.request_ms_total += int((time.perf_counter() - t0) * 1000)

        try:
            payload = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            raise BackendDecodeError(
                f"transcription endpoint returned {len(raw)} bytes that are not JSON"
            ) from exc

        text = payload.get("text") if isinstance(payload, dict) else None
        if not isinstance(text, str):
            raise BackendDecodeError(
                f"transcription response has no string 'text' field (keys: "
                f"{sorted(payload) if isinstance(payload, dict) else type(payload).__name__})"
            )
        # An empty string is a legitimate answer — silence, or audio below the model's threshold —
        # and must not be treated as an error. Counted so a session that produced nothing at all
        # can be told apart from one that was never asked.
        if not text.strip():
            self.empty_response_count += 1
        return text.strip()

    # -- diagnostics ---------------------------------------------------------------------------

    def describe(self) -> dict:
        d = super().describe()
        d.update(
            {
                "endpoint": self._base_url + _PATH,
                "requests": self.request_count,
                "request_ms_total": self.request_ms_total,
                "request_ms_mean": (
                    self.request_ms_total // self.request_count if self.request_count else 0
                ),
                "empty_responses": self.empty_response_count,
                "finalize_only": self._finalize_only,
            }
        )
        return d
