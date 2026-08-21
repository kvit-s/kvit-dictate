"""`--backend windows-speech`: the on-device Windows AI speech model, via a helper process.

The model is Whisper Small, quantized to int8, shipped by Microsoft as an optional Windows
component and driven through ``Microsoft.Windows.AI.Speech``. Everything runs on this machine, so
unlike ``openai-http`` this backend keeps plan.md §2's "no network access during dictation"
guarantee in the letter as well as the spirit.

─── Why there is a helper process at all ───────────────────────────────────────────────────────

``Microsoft.Windows.AI.Speech`` is a WinRT API delivered by the Windows App SDK. Activating it
from CPython would mean a WinRT projection and a Windows App SDK bootstrapper in the sidecar's
interpreter — which would end this package's stdlib-only rule for every backend, not just this
one. So the WinRT lives in ``tools/kvit-speech`` (C#) and this module talks to it over a small
framed protocol on stdin/stdout. **This module stays stdlib-only**: subprocess, struct, threading.

The helper is long-lived because model creation is expensive and per-utterance spawning would pay
it every time (measured 2851 ms cold / 919 ms warm).

─── Why this is a PseudoStreamingBackend, and why finalize-only is the default ─────────────────

The API's streaming mode was measured and rejected for this seam, not overlooked.
``StreamingRecognition`` emits ``Recognized`` events carrying **overlapping four-second windows**
whose text repeats across the seam, recased:

    off=0     dur=4000   "The quick brown fox jumps over the lazy dog. This"
    off=3960  dur=4000   " is a test of the Windows on-device speech recognition model."
    off=7960  dur=1120   " of the Windows on Device Speech Recognition Model."

docs/protocol.md §8.1 requires a backend's text to be **cumulative**, and reconciling those
windows into a cumulative string means a de-overlap heuristic whose failures would look exactly
like the stability violations §8.3 exists to reject. ``Recognizing`` never fired at all, and
``IsFinal`` was never true. Batch recognition returns one clean, correctly-cased, correctly-
punctuated string for the window it is given, which is what ``PseudoStreamingBackend`` is built
around. If the streaming API's semantics change, revisit — but re-measure first.

Decode cost, measured on an i7-14700K with the CPU execution provider:

    audio     500 ms   1 s     2 s     3 s     5 s     8 s     15 s     30 s
    decode   1512 ms  1668   2112    2203    2984    3686    5715    10197

That is roughly **1350 ms of fixed cost plus 0.30× real time**. The fixed floor is what decides
the default: a one-second hop would need a decode per second and each one costs 1.7 s, so a live
preview cannot keep up and would fall further behind with every hop. ``finalize_only`` is
therefore **on by default** — one decode per utterance, at the endpoint — which is the same shape
as the `qwen-asr-http-finalize` entry. Turning it off is supported and will produce a revising
preview; it will also lag, and that is a property of the model, not of a setting.

─── The silence gate is a correctness requirement ──────────────────────────────────────────────

This is Whisper, and it hallucinates over silence exactly as plan.md §1.6 warns. Measured: two
seconds of **digital silence** returned

    "Rwy'n cael ei wneud i'r unrhyw ychydig."

— a fluent Welsh sentence, from an all-zero buffer. The server's VAD already gates the decoder, so
in a normal session this window is never built. ``silence_peak_threshold`` is the second line of
that defence, inside the backend, where it also saves the 1.4 s a silent decode would cost.
Setting it to 0 disables the gate and is only correct in a test.
"""

from __future__ import annotations

import os
import queue
import struct
import subprocess
import threading
import time
from pathlib import Path
from typing import Any, Optional

from .base import (
    BackendCapabilities,
    BackendDecodeError,
    BackendLoadError,
    ProgressFn,
    PseudoStreamingBackend,
    ResetReason,
    StreamingMode,
    bytes_to_ms,
    pcm_peak,
)

__all__ = ["WindowsSpeechBackend"]

# ─── The helper protocol (tools/kvit-speech/Program.cs carries the normative copy) ───────────
#
#     [u32 le length][u8 tag][payload...]      length counts the tag byte and the payload
#
# Requests: 'T' transcribe (payload = s16le PCM), 'P' ping, 'Q' quit.
# Responses: 'R' ready (once, unsolicited), 'O' ok (payload = UTF-8 text), 'E' error.

_TAG_TRANSCRIBE = b"T"
_TAG_PING = b"P"
_TAG_QUIT = b"Q"

_MAX_FRAME_BYTES = 32 * 1024 * 1024

# Where to look for the helper when the model entry does not name one. Relative to the repository
# root, which is the parent of the directory holding the `kvit_asr` package.
_EXE_NAME = "kvit-speech.exe"
_EXE_SEARCH = (
    "tools/kvit-speech/dist",
    "tools/kvit-speech/bin/Release/net9.0-windows10.0.26100.0/win-x64",
    "tools/kvit-speech/bin/Debug/net9.0-windows10.0.26100.0/win-x64",
)


def _repo_root() -> Path:
    # .../server/kvit_asr/backends/windows_speech.py -> .../  (four parents up)
    return Path(__file__).resolve().parents[3]


def find_helper() -> Optional[Path]:
    """The helper executable, or ``None``. Env var first, then the build outputs.

    ``KVIT_SPEECH_EXE`` exists so a published copy somewhere else can be used without editing
    models.json — the same escape hatch `PYTHONPATH` gives the sidecar itself.
    """
    from_env = os.environ.get("KVIT_SPEECH_EXE")
    if from_env:
        p = Path(from_env)
        return p if p.is_file() else None

    root = _repo_root()
    for rel in _EXE_SEARCH:
        p = root / rel / _EXE_NAME
        if p.is_file():
            return p
    return None


class _Helper:
    """The child process and the one thread that reads from it.

    A dedicated reader thread rather than blocking reads on the calling thread, because a helper
    that wedges must become a timeout rather than a hung dictation session. ``feed()`` is
    synchronous by the Backend contract, so a stuck read would stall the audio path with the user
    still talking and no way to find out why.
    """

    def __init__(self, exe: Path, args: list[str], stderr_lines: int = 40) -> None:
        self._exe = exe
        # CREATE_NO_WINDOW: the sidecar is already windowless and a console flashing up per
        # dictation session would be a visible defect.
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self._proc = subprocess.Popen(
            [str(exe), *args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            cwd=str(exe.parent),
            creationflags=creationflags,
        )
        self._frames: "queue.Queue[tuple[str, bytes] | BaseException]" = queue.Queue()
        self._stderr: list[str] = []
        self._stderr_max = stderr_lines
        self._stderr_lock = threading.Lock()
        self._dead: Optional[str] = None

        self._reader = threading.Thread(target=self._read_frames, name="kvit-speech-read", daemon=True)
        self._reader.start()
        self._draining = threading.Thread(target=self._drain_stderr, name="kvit-speech-err", daemon=True)
        self._draining.start()

    # -- the two threads ----------------------------------------------------------------------

    def _read_frames(self) -> None:
        stdout = self._proc.stdout
        assert stdout is not None
        try:
            while True:
                head = self._read_exactly(stdout, 4)
                if head is None:
                    raise EOFError("the helper closed its output stream")
                (length,) = struct.unpack("<I", head)
                if not 1 <= length <= _MAX_FRAME_BYTES:
                    raise ValueError(f"helper sent a frame length of {length}; the stream is desynchronised")
                body = self._read_exactly(stdout, length)
                if body is None:
                    raise EOFError(f"the helper's output ended {length} bytes into a frame")
                self._frames.put((chr(body[0]), bytes(body[1:])))
        except BaseException as exc:  # noqa: BLE001 - handed to the caller, never swallowed
            self._frames.put(exc)

    @staticmethod
    def _read_exactly(stream: Any, n: int) -> Optional[bytearray]:
        buf = bytearray()
        while len(buf) < n:
            chunk = stream.read(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def _drain_stderr(self) -> None:
        stderr = self._proc.stderr
        assert stderr is not None
        for raw in stderr:
            line = raw.decode("utf-8", "replace").rstrip()
            if not line:
                continue
            with self._stderr_lock:
                self._stderr.append(line)
                # Bounded: a helper that logs in a loop must not become a memory leak in a process
                # whose whole job is to stay up for the length of a dictation session.
                if len(self._stderr) > self._stderr_max:
                    del self._stderr[: len(self._stderr) - self._stderr_max]

    # -- the interface the backend uses --------------------------------------------------------

    def recent_stderr(self, limit: int = 8) -> str:
        with self._stderr_lock:
            return "\n".join(self._stderr[-limit:])

    def request(self, tag: bytes, payload: bytes, timeout_s: float) -> tuple[str, bytes]:
        if self._dead is not None:
            raise BackendDecodeError(f"the kvit-speech helper is no longer usable: {self._dead}")
        stdin = self._proc.stdin
        assert stdin is not None
        try:
            stdin.write(struct.pack("<I", len(payload) + 1) + tag + payload)
            stdin.flush()
        except OSError as exc:
            self._die(f"writing to the helper failed ({type(exc).__name__}: {exc})")
            raise BackendDecodeError(self._dead or str(exc)) from exc
        return self.receive(timeout_s)

    def receive(self, timeout_s: float) -> tuple[str, bytes]:
        try:
            item = self._frames.get(timeout=timeout_s)
        except queue.Empty:
            # The answer may still be in flight, so the stream can no longer be trusted to be in
            # step. Killing the helper is the honest response: a late frame answering a request
            # the caller has given up on would be handed to the *next* caller as its own.
            self._die(f"no answer within {timeout_s:.1f} s")
            raise BackendDecodeError(
                f"the kvit-speech helper did not answer within {timeout_s:.1f} s"
                + self._stderr_suffix()
            ) from None
        if isinstance(item, BaseException):
            self._die(f"{type(item).__name__}: {item}")
            raise BackendDecodeError(
                f"the kvit-speech helper stopped responding ({type(item).__name__}: {item})"
                + self._stderr_suffix()
            ) from item
        return item

    def _stderr_suffix(self) -> str:
        tail = self.recent_stderr()
        return f"\n--- last output from the helper ---\n{tail}" if tail else ""

    def _die(self, why: str) -> None:
        if self._dead is None:
            self._dead = why
        self.terminate()

    def terminate(self, grace_s: float = 3.0) -> None:
        if self._proc.poll() is not None:
            return
        try:
            if self._proc.stdin is not None and not self._proc.stdin.closed:
                try:
                    self._proc.stdin.write(struct.pack("<I", 1) + _TAG_QUIT)
                    self._proc.stdin.flush()
                except OSError:
                    pass
                try:
                    self._proc.stdin.close()
                except OSError:
                    pass
            self._proc.wait(timeout=grace_s)
        except (subprocess.TimeoutExpired, OSError):
            try:
                self._proc.kill()
            except OSError:
                pass


class WindowsSpeechBackend(PseudoStreamingBackend):
    """`--backend windows-speech`. See the module docstring for what this is and is not for."""

    def __init__(
        self,
        *,
        exe: Optional[str] = None,
        window_ms: int = 15000,
        hop_ms: int = 1000,
        finalize_only: bool = True,
        timeout_s: float = 60.0,
        startup_timeout_s: float = 60.0,
        silence_peak_threshold: int = 200,
        ensure_model: bool = False,
    ) -> None:
        # Same device as openai-http's: a hop no shorter than the window means the only decode is
        # the one finalize() forces, which is one request per utterance and no live preview.
        if finalize_only:
            hop_ms = max(hop_ms, window_ms)
        super().__init__(window_ms=window_ms, hop_ms=hop_ms)
        self._exe_option = exe
        self._exe: Optional[Path] = None
        self._finalize_only = bool(finalize_only)
        self._timeout_s = float(timeout_s)
        self._startup_timeout_s = float(startup_timeout_s)
        self._silence_peak_threshold = int(silence_peak_threshold)
        self._ensure_model = bool(ensure_model)
        self._helper: Optional[_Helper] = None
        self._native_folder = ""

        # Counters a result file can carry, so a slow run is attributable afterwards.
        self.request_count = 0
        self.request_ms_total = 0
        self.empty_response_count = 0
        self.silence_skipped_count = 0

    # -- declaration --------------------------------------------------------------------------

    @property
    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(
            model_id="windows-ai-speech",
            mode=StreamingMode.PSEUDO_STREAMING,
            supports_context=False,
            natural_chunk_ms=self.hop_ms,
            chunk_ms_configurable=True,
            # Every decode is independent: the window is handed over whole and the helper keeps no
            # state between calls, so a gap in the audio costs content but never coherence.
            requires_contiguous_audio=False,
            notes=(
                "Windows AI Speech (Whisper Small int8) on-device via tools/kvit-speech; "
                f"{'finalize-only' if self._finalize_only else f'hop={self.hop_ms}ms'}; "
                f"window={self.window_ms}ms. Batch model behind a streaming interface, so its "
                "latency is a property of this CPU and not of an ASR architecture, and must not "
                "be reported in plan.md section 4. Decode costs about 1350 ms fixed plus 0.30x "
                "real time. Hallucinates over silence; the VAD gate is required."
            ),
        )

    # -- construction from the command line ---------------------------------------------------

    @classmethod
    def from_backend_config(cls, cfg: Any) -> "WindowsSpeechBackend":
        options: dict[str, Any] = {}
        if getattr(cfg, "chunk_ms", None) is not None:
            options["hop_ms"] = cfg.chunk_ms
        for key, value in (getattr(cfg, "options", None) or {}).items():
            options[key] = value
        for flag in ("finalize_only", "ensure_model"):
            if flag in options and isinstance(options[flag], str):
                options[flag] = options[flag].strip().lower() in ("1", "true", "yes", "on")
        for num in ("hop_ms", "window_ms", "silence_peak_threshold"):
            if num in options and isinstance(options[num], str):
                options[num] = int(options[num])
        for real in ("timeout_s", "startup_timeout_s"):
            if real in options and isinstance(options[real], str):
                options[real] = float(options[real])
        return cls(**options)

    # -- lifecycle ----------------------------------------------------------------------------

    def _load(self, progress: Optional[ProgressFn]) -> None:
        self._report(progress, "locate", 0)
        exe = Path(self._exe_option) if self._exe_option else find_helper()
        if exe is None or not exe.is_file():
            raise BackendLoadError(
                f"cannot find {_EXE_NAME}. Build it with "
                f"`dotnet build -c Release tools/kvit-speech` (it needs the .NET 9 SDK), or point "
                f"the KVIT_SPEECH_EXE environment variable at an existing copy. Looked in: "
                + ", ".join(str(_repo_root() / r) for r in _EXE_SEARCH)
                + (f"; the entry named {self._exe_option!r}" if self._exe_option else "")
            )
        self._exe = exe

        self._report(progress, "spawn", 10)
        args = ["--ensure-model"] if self._ensure_model else []
        try:
            helper = _Helper(exe, args)
        except OSError as exc:
            raise BackendLoadError(
                f"cannot start {exe} ({type(exc).__name__}: {exc})"
            ) from exc

        # The helper sends exactly one unsolicited frame once the model is created: 'R' with a
        # JSON description, or 'E' with the reason it could not. Both are useful; neither is a
        # traceback.
        self._report(progress, "model", 40)
        try:
            tag, payload = helper.receive(self._startup_timeout_s)
        except BackendDecodeError as exc:
            helper.terminate()
            raise BackendLoadError(str(exc)) from exc

        if tag == "E":
            detail = payload.decode("utf-8", "replace")
            helper.terminate()
            raise BackendLoadError(f"the Windows AI speech model is unavailable: {detail}")
        if tag != "R":
            helper.terminate()
            raise BackendLoadError(
                f"the helper's first frame was {tag!r}, expected 'R' (ready) or 'E' (error)"
            )

        # Parsed loosely on purpose: the only field this side uses is a diagnostic, and a helper
        # that grows a field must not fail a load here.
        text = payload.decode("utf-8", "replace")
        marker = '"native_folder":"'
        if marker in text:
            self._native_folder = text.split(marker, 1)[1].split('"', 1)[0].replace("\\\\", "\\")

        self._helper = helper
        self._report(progress, "model", 100)

    def _unload(self) -> None:
        if self._helper is not None:
            self._helper.terminate()
            self._helper = None

    def _reset(self, reason: ResetReason) -> None:
        # The helper is stateless between decodes, so there is nothing to tell it. The window that
        # PseudoStreamingBackend keeps is the only state, and the base class clears that.
        super()._reset(reason)

    # -- the one thing PseudoStreamingBackend asks for -----------------------------------------

    def _transcribe_batch(self, pcm: bytes) -> str:
        if not pcm:
            return ""

        if self._silence_peak_threshold > 0 and pcm_peak(pcm) < self._silence_peak_threshold:
            # Not a failure: "this window produced no hypothesis" is exactly right, and it is also
            # the answer that stops Whisper inventing a sentence over an empty buffer.
            self.silence_skipped_count += 1
            return ""

        helper = self._helper
        if helper is None:
            raise BackendDecodeError("the kvit-speech helper is not running")

        t0 = time.perf_counter()
        try:
            tag, payload = helper.request(_TAG_TRANSCRIBE, pcm, self._timeout_s)
        finally:
            self.request_count += 1
            self.request_ms_total += int((time.perf_counter() - t0) * 1000)

        if tag == "E":
            raise BackendDecodeError(
                f"the Windows AI speech model failed on {bytes_to_ms(len(pcm))} ms of audio: "
                + payload.decode("utf-8", "replace")
            )
        if tag != "O":
            raise BackendDecodeError(f"the helper answered {tag!r}, expected 'O' or 'E'")

        text = payload.decode("utf-8", "replace").strip()
        # An empty string is a legitimate answer and must not look like an error.
        if not text:
            self.empty_response_count += 1
        return text

    # -- diagnostics ---------------------------------------------------------------------------

    def describe(self) -> dict:
        d = super().describe()
        d.update(
            {
                "helper": str(self._exe) if self._exe else None,
                "native_folder": self._native_folder,
                "requests": self.request_count,
                "request_ms_total": self.request_ms_total,
                "request_ms_mean": (
                    self.request_ms_total // self.request_count if self.request_count else 0
                ),
                "empty_responses": self.empty_response_count,
                "silence_skipped": self.silence_skipped_count,
                "finalize_only": self._finalize_only,
            }
        )
        return d
