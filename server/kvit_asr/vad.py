# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Voice activity detection and **utterance** endpointing for the kvit-dictate sidecar.

Normative sources: plan.md §1.6 (session lifetime and silence) and §3 Phase 2 step 3. Where this
module and plan.md disagree, plan.md wins and this module is the bug.

─── Scope: this module ends UTTERANCES, never SESSIONS ─────────────────────────────────────────

plan.md §1.6 names two timers and is emphatic that a naive implementation merges them. Merging
them *is* the specific `Win+H` failure this product exists to fix — Win+H stops the whole
dictation after a short silence and offers no setting to change it, so a thinking pause ends the
session and the user has to press the hotkey again mid-sentence.

    | Timer            | Default | Owner        | Ends       |
    |------------------|---------|--------------|------------|
    | Endpoint silence | ~0.8 s  | **this file**| an utterance — finalise, punctuate, `final` |
    | Session silence  | 120 s   | the C++ side | the session — mic closes, overlay goes away |

This module implements **only the first row**. It has no concept of a session, holds no session
timer, and nothing in here may ever be wired to one. A minute-long pause must cost nothing: the
utterance endpoints at 0.8 s, the session stays open, and resuming starts a new utterance. If a
future change makes this module able to end a session, that change is wrong.

─── Decoder gating is a correctness requirement, not an optimisation ───────────────────────────

plan.md §1.6: *"Do not feed 60 seconds of silence to a streaming model — several ASR models
hallucinate on it. `wan-dancer`'s own `notes/transcribe.py` carries a comment about Whisper
emitting 'Thank you.' / 'Bye' over trailing silence with timestamps past the end of the audio."*

So the session loop must **stop feeding the decoder and reset its state** at the endpoint, and
resume on the next speech onset. `VadChunk.segments` is that signal in its usable form: it
carries exactly the audio that should reach the decoder, tagged with the utterance it belongs
to, and it is empty while the gate is shut. A change of `AudioSegment.utterance_index` is the
instruction to reset decoder state before feeding.

Two deliberate consequences:

* The gate stays **open across a short mid-sentence pause** (shorter than the endpoint timer).
  The decoder needs to hear that pause to place the word boundary and the punctuation, and
  ~0.8 s of trailing silence is nowhere near the tens of seconds that provoke hallucination.
* The gate opens `preroll_ms` *before* the detected onset. Any frame-level detector is late by
  construction — it cannot know a run is speech until it has heard enough of it — and clipping
  the first phoneme is a worse error than feeding a fraction of a second of room tone.

─── Two backends behind one interface ──────────────────────────────────────────────────────────

The endpointing state machine is backend-independent: a backend's only job is to score one frame
in [0, 1], and `VadEndpointer` turns a stream of scores into events.

1. `SileroOnnxScorer` — the intended production path (bench/CANDIDATES.md picks ONNX Runtime as
   the runtime that matters, and `onnx-community/nemotron-3.5-asr-streaming-0.6b-onnx-int4` even
   bundles a `silero_vad.onnx`). `onnxruntime` and `numpy` are imported **inside** `load()`, and
   the model path comes from configuration — nothing here downloads anything (plan.md §1.9: no
   model auto-download at runtime).
2. `EnergyZcrScorer` — stdlib only, no numpy. It exists so the sidecar has working endpointing on
   a bare interpreter. It is **worse**, in ways enumerated in its own docstring; it is not a
   substitute for Silero and no measurement taken with it should be reported as if it were.

`create_vad()` resolves between them and logs one clear line when it falls back.

─── Units ─────────────────────────────────────────────────────────────────────────────────────

Positions are **sample counts** on the stream this instance has been fed (`sample` on every
event, `start_sample` on every segment), converted to milliseconds only at the edges. Samples are
the sample-accurate unit; milliseconds are lossy at 16 kHz and would make event positions
untestable. Audio crossing this module's boundary is the same format as the wire
(docs/protocol.md §6.3): 16 kHz mono **s16le**, even byte counts.

This module contains no text and therefore no UTF-8 byte offsets — the grapheme/offset hazard the
repository rules warn about lives in the stability computation, not here.

Threading: a `VadEndpointer` is single-owner and **not** thread-safe. In the sidecar that owner is
the session loop.
"""

from __future__ import annotations

import array
import logging
import math
import os
import sys
from dataclasses import dataclass
from enum import StrEnum

__all__ = [
    "BYTES_PER_SAMPLE",
    "AudioSegment",
    "EnergyZcrScorer",
    "FrameScorer",
    "SileroOnnxScorer",
    "VadBackendUnavailable",
    "VadChunk",
    "VadConfig",
    "VadConfigError",
    "VadEndpointer",
    "VadEvent",
    "VadEventKind",
    "VadState",
    "create_vad",
    "ms_to_samples",
    "pcm_to_samples",
    "samples_to_ms",
    "samples_to_pcm",
]

logger = logging.getLogger(__name__)

BYTES_PER_SAMPLE = 2

# `array('h')` is C `short`, which the C standard only guarantees to be *at least* 16 bits. Every
# platform this project targets has a 2-byte short, but the wire format is fixed at s16le
# (docs/protocol.md §6.3), so a wider short would silently misparse every audio frame. Fail at
# import rather than produce plausible garbage.
if array.array("h").itemsize != BYTES_PER_SAMPLE:
    raise RuntimeError(
        "array('h').itemsize is %d, expected %d; this platform cannot represent the s16le wire "
        "format with the 'h' typecode" % (array.array("h").itemsize, BYTES_PER_SAMPLE)
    )

_FULL_SCALE = 32768.0

# Digital silence has no dB value. Clamping instead of returning -inf keeps every downstream
# arithmetic op finite, and -120 dBFS is far below anything a 16-bit capture can express anyway.
_DBFS_FLOOR = -120.0


class VadBackendUnavailable(RuntimeError):
    """A VAD backend cannot be used *in this environment* — missing runtime, missing model file.

    Distinct from `VadConfigError` on purpose: this one is recoverable by falling back to another
    backend, and `create_vad()` does exactly that. A configuration mistake is not.
    """


class VadConfigError(ValueError):
    """The configuration is self-inconsistent or impossible.

    Never silently recovered from. A bad `frame_samples` for Silero is a deployment mistake, not
    an unavailable runtime, and quietly running the fallback would hide it forever.
    """


# ─── Time and PCM helpers ───────────────────────────────────────────────────────────────────────


def ms_to_samples(ms: float, sample_rate: int) -> int:
    """Milliseconds → whole samples, rounded to nearest. Never negative."""
    return max(0, int(round(ms * sample_rate / 1000.0)))


def samples_to_ms(samples: int, sample_rate: int) -> float:
    """Samples → milliseconds as a float. Callers that put this on the wire must round to int:
    docs/protocol.md §4.2 defines every protocol number as an integer."""
    return samples * 1000.0 / sample_rate


def pcm_to_samples(pcm: bytes) -> array.array:
    """s16le bytes → an `array('h')` in **native** byte order.

    The byteswap is not theoretical tidiness: `array.frombytes` reads native order, the wire is
    little-endian by specification, and on a big-endian host the difference is the whole signal.
    Converting once here means every scorer downstream can treat the array as plain integers.
    """
    if len(pcm) % BYTES_PER_SAMPLE:
        # docs/protocol.md §6.3 makes an odd-length audio payload `bad_message` at the protocol
        # layer; this is the defensive echo of that rule, because a half sample here would shift
        # every subsequent frame and show up only as mysteriously bad transcription.
        raise ValueError(
            "PCM payload length %d is odd; s16le samples cannot be split" % len(pcm)
        )
    out = array.array("h")
    out.frombytes(pcm)
    if sys.byteorder != "little":
        out.byteswap()
    return out


def samples_to_pcm(samples: array.array) -> bytes:
    """`array('h')` in native order → s16le bytes, ready for a decoder or the wire."""
    if sys.byteorder != "little":
        swapped = array.array("h", samples)
        swapped.byteswap()
        return swapped.tobytes()
    return samples.tobytes()


# ─── Configuration ──────────────────────────────────────────────────────────────────────────────


@dataclass
class VadConfig:
    """Every tunable in one place. Defaults are the plan's, or reasoned guesses labelled as such.

    None of these numbers has been measured on this project's corpus yet — the corpus does not
    exist until Phase 1 and the VAD sweep is Phase 2. They are starting points chosen to be
    defensible, not results.
    """

    # ── Stream format ──────────────────────────────────────────────────────────────────────────
    sample_rate: int = 16000
    # 512 samples = 32 ms. Chosen because Silero's published 16 kHz usage requires exactly this
    # window (see `SileroOnnxScorer`), and using the same hop for both backends keeps event
    # positions comparable between them. It also bounds the position error of every event at one
    # frame, which is what the tests assert against.
    frame_samples: int = 512

    # ── Frame decision ─────────────────────────────────────────────────────────────────────────
    # Trigger at `threshold`, un-trigger at `neg_threshold`. The hysteresis gap is what stops a
    # probability hovering at the boundary from producing a burst of one-frame speech segments.
    threshold: float = 0.5
    neg_threshold: float = 0.35

    # ── Endpointing ────────────────────────────────────────────────────────────────────────────
    # A speech run shorter than this never produces an onset: it is a click, a lip smack, a chair.
    # Suppression is on the *onset*, not on the endpoint timer — see `VadEndpointer._on_frame`.
    min_speech_ms: float = 200.0
    # How long speech must be absent before a SPEECH_END is reported. Below this, a pause is
    # invisible: no event, no state change. This is the "brief pause mid-sentence" knob.
    hangover_ms: float = 250.0
    # plan.md §1.6's ~0.8 s. Ends the UTTERANCE. Never the session.
    endpoint_silence_ms: float = 800.0
    # Audio fed to the decoder ahead of the detected onset. See the module docstring.
    preroll_ms: float = 300.0
    # Safety valve, not a product feature. An utterance that never endpoints (continuous noise
    # that keeps resetting the timer) would feed a streaming decoder without bound and never
    # produce a `final`. 30 s is well inside the C++ side's 120 s session timer, so this can never
    # be mistaken for the session timer. 0 disables it.
    max_utterance_ms: float = 30000.0

    # ── Backend selection ──────────────────────────────────────────────────────────────────────
    # "auto"   — try Silero, fall back to energy with a warning
    # "silero" — Silero or nothing (raises); for benchmarking, where a silent fallback would
    #            corrupt the measurement
    # "energy" — the stdlib fallback, explicitly
    backend: str = "auto"
    # Absolute path to `silero_vad.onnx`. **Nothing here downloads it** (plan.md §1.9). Absent
    # path or absent file is reported precisely, with the path in the message.
    silero_model_path: str | None = None
    # ONNX Runtime execution providers, most-preferred first. CPU by default: bench/CANDIDATES.md
    # §6.1 records that `onnxruntime-gpu` on Blackwell/sm_120 is unconfirmed, and a VAD costs
    # microseconds — there is nothing here worth a GPU.
    silero_providers: tuple[str, ...] = ("CPUExecutionProvider",)
    silero_intra_op_threads: int = 1

    # ── Energy/ZCR fallback ────────────────────────────────────────────────────────────────────
    # SNR above the tracked noise floor at which the pseudo-probability crosses 0.5 — i.e. at
    # which the default `threshold` triggers. The mapping is deliberate: changing `threshold`
    # moves both backends in the same direction.
    energy_snr_db: float = 10.0
    energy_snr_slope_db: float = 3.0
    # Absolute gate. Without it, a noise floor tracked down to digital silence makes -70 dBFS
    # dither look like a 50 dB SNR event.
    energy_abs_floor_dbfs: float = -55.0
    # The floor is calibrated from the quietest frame in this opening window, during which the
    # scorer reports 0.0. Cost: the fallback is deaf for the first fifth of a second of a stream.
    # Benefit: a stream that opens on steady room noise does not report that noise as speech.
    # Silero has no such wart; this is one of the ways the fallback is worse.
    energy_warmup_ms: float = 200.0
    energy_initial_noise_dbfs: float = -75.0
    # Asymmetric adaptation: drop to a new quieter floor fast, rise to a louder one slowly, and
    # only on frames not already judged speech. A symmetric filter lets sustained speech drag the
    # floor up until the speech stops being speech.
    energy_noise_down: float = 0.5
    energy_noise_up: float = 0.05
    # Zero-crossing band treated as plausible unvoiced speech (/s/, /f/, /ʃ/). Voiced speech sits
    # well below it; white noise sits above it at ~0.5. Weak feature — see `EnergyZcrScorer`.
    zcr_min: float = 0.15
    zcr_max: float = 0.45
    zcr_boost_db: float = 4.0

    def __post_init__(self) -> None:
        if self.sample_rate <= 0:
            raise VadConfigError("sample_rate must be positive, got %r" % (self.sample_rate,))
        if self.frame_samples <= 1:
            raise VadConfigError("frame_samples must be > 1, got %r" % (self.frame_samples,))
        if not 0.0 < self.threshold <= 1.0:
            raise VadConfigError("threshold must be in (0, 1], got %r" % (self.threshold,))
        if not 0.0 <= self.neg_threshold < self.threshold:
            raise VadConfigError(
                "neg_threshold must be in [0, threshold), got %r with threshold %r"
                % (self.neg_threshold, self.threshold)
            )
        for name in ("min_speech_ms", "hangover_ms", "endpoint_silence_ms", "preroll_ms"):
            if getattr(self, name) < 0:
                raise VadConfigError("%s must be >= 0, got %r" % (name, getattr(self, name)))
        if self.endpoint_silence_ms <= 0:
            raise VadConfigError("endpoint_silence_ms must be > 0")
        if self.hangover_ms > self.endpoint_silence_ms:
            # A hangover longer than the endpoint would report the end of speech *after* the
            # utterance it belongs to had already been closed.
            raise VadConfigError(
                "hangover_ms (%r) must not exceed endpoint_silence_ms (%r)"
                % (self.hangover_ms, self.endpoint_silence_ms)
            )
        if self.max_utterance_ms and self.max_utterance_ms <= self.endpoint_silence_ms:
            raise VadConfigError(
                "max_utterance_ms (%r) must exceed endpoint_silence_ms (%r) or be 0 to disable"
                % (self.max_utterance_ms, self.endpoint_silence_ms)
            )
        if self.backend not in ("auto", "silero", "energy"):
            raise VadConfigError(
                "backend must be one of 'auto', 'silero', 'energy'; got %r" % (self.backend,)
            )

    # Convenience conversions, so the state machine never repeats the arithmetic.
    @property
    def frame_ms(self) -> float:
        return samples_to_ms(self.frame_samples, self.sample_rate)

    @property
    def min_speech_samples(self) -> int:
        return ms_to_samples(self.min_speech_ms, self.sample_rate)

    @property
    def hangover_samples(self) -> int:
        return ms_to_samples(self.hangover_ms, self.sample_rate)

    @property
    def endpoint_samples(self) -> int:
        return ms_to_samples(self.endpoint_silence_ms, self.sample_rate)

    @property
    def preroll_samples(self) -> int:
        return ms_to_samples(self.preroll_ms, self.sample_rate)

    @property
    def max_utterance_samples(self) -> int:
        return ms_to_samples(self.max_utterance_ms, self.sample_rate)


# ─── Events and output ──────────────────────────────────────────────────────────────────────────


class VadEventKind(StrEnum):
    """`StrEnum` so a log line prints `speech_start`, not `VadEventKind.SPEECH_START`."""

    SPEECH_START = "speech_start"
    SPEECH_END = "speech_end"
    UTTERANCE_END = "utterance_end"
    DISCONTINUITY = "discontinuity"


class VadState(StrEnum):
    SILENCE = "silence"
    SPEECH = "speech"


@dataclass(frozen=True)
class VadEvent:
    """One endpointing decision, with the position it is *about* and the position it was *made*.

    The two differ by construction and conflating them is the classic VAD reporting bug:
    `sample` is where speech actually started or stopped (backdated — that is the number a
    transcript timeline or `final.audio_pos_ms` wants), while `decided_at_sample` is where the
    detector had enough evidence to say so (that is the number a latency measurement wants).
    plan.md §3 Phase 2 item 4 requires endpoint delay to be measured separately from the rest, so
    both are recorded rather than one being derived and lost.
    """

    kind: VadEventKind
    sample: int
    decided_at_sample: int
    utterance_index: int
    sample_rate: int
    reason: str = ""

    @property
    def ms(self) -> float:
        return samples_to_ms(self.sample, self.sample_rate)

    @property
    def decided_at_ms(self) -> float:
        return samples_to_ms(self.decided_at_sample, self.sample_rate)

    @property
    def latency_ms(self) -> float:
        """How late the decision was relative to the thing it describes."""
        return samples_to_ms(self.decided_at_sample - self.sample, self.sample_rate)


@dataclass(frozen=True)
class AudioSegment:
    """A contiguous run of audio the decoder should receive, tagged with its utterance.

    `utterance_index` is what makes decoder gating implementable without a second channel: when
    it differs from the previous segment's, the session loop MUST reset decoder state before
    feeding this one. Utterance indices are 1-based to line up with docs/protocol.md §7.3, which
    starts `utterance_id` at 1 — but note this module counts utterances in *its own* stream and
    has no session concept, so the mapping is the session loop's to make.
    """

    utterance_index: int
    start_sample: int
    pcm: bytes

    @property
    def n_samples(self) -> int:
        return len(self.pcm) // BYTES_PER_SAMPLE

    @property
    def end_sample(self) -> int:
        return self.start_sample + self.n_samples


@dataclass(frozen=True)
class VadChunk:
    """Everything one `process()`/`flush()`/`notify_discontinuity()` call produced."""

    events: tuple[VadEvent, ...] = ()
    segments: tuple[AudioSegment, ...] = ()
    gate_open: bool = False

    @property
    def decoder_audio(self) -> bytes:
        """All segment audio concatenated — a convenience for the common case.

        **Only safe when `len(segments) <= 1`.** If a call spans an utterance boundary the joined
        bytes straddle a required decoder reset, and feeding them as one block is precisely the
        state leak the gate exists to prevent. Callers that feed audio chunk-by-chunk at the
        protocol's recommended 20–100 ms (docs/protocol.md §6.3) will never see more than one
        segment; callers that push whole seconds at once must iterate `segments`.
        """
        if len(self.segments) == 1:
            return self.segments[0].pcm
        return b"".join(seg.pcm for seg in self.segments)

    @property
    def reset_decoder(self) -> bool:
        """True if the decoder's state is invalid from here on: an utterance ended, or the audio
        stream was spliced. Also implied by any change of `AudioSegment.utterance_index`."""
        return any(
            e.kind in (VadEventKind.UTTERANCE_END, VadEventKind.DISCONTINUITY)
            for e in self.events
        )

    def of_kind(self, kind: VadEventKind) -> tuple[VadEvent, ...]:
        return tuple(e for e in self.events if e.kind is kind)


class _SegmentBuilder:
    """Accumulates frame audio into as few `AudioSegment`s as the utterance tagging allows."""

    def __init__(self) -> None:
        self._segments: list[list] = []  # [utterance_index, start_sample, bytearray]

    def add(self, utterance_index: int, start_sample: int, samples: array.array) -> None:
        if not samples:
            return
        if self._segments:
            last = self._segments[-1]
            contiguous = last[1] + len(last[2]) // BYTES_PER_SAMPLE == start_sample
            if last[0] == utterance_index and contiguous:
                last[2].extend(samples_to_pcm(samples))
                return
        self._segments.append([utterance_index, start_sample, bytearray(samples_to_pcm(samples))])

    def finish(self) -> tuple[AudioSegment, ...]:
        return tuple(
            AudioSegment(utterance_index=u, start_sample=s, pcm=bytes(b))
            for u, s, b in self._segments
        )


# ─── Frame scorers ──────────────────────────────────────────────────────────────────────────────


class FrameScorer:
    """Scores one frame of audio in [0, 1]. The whole backend interface.

    Implementations are stateful across frames (an RNN's hidden state, an adaptive noise floor)
    and single-owner. `reset()` means "the audio stream is no longer continuous" — after a WASAPI
    discontinuity, not after every utterance. Utterance boundaries deliberately do *not* reset a
    scorer: the noise floor and the acoustic context of the room are still valid, and throwing
    them away would make the first utterance after every pause behave like the first utterance
    of the stream.
    """

    name = "abstract"

    def score(self, frame: array.array) -> float:
        raise NotImplementedError

    def reset(self) -> None:
        pass

    def close(self) -> None:
        pass


def _logistic(x: float) -> float:
    if x >= 0.0:
        return 1.0 / (1.0 + math.exp(-min(x, 60.0)))
    return math.exp(max(x, -60.0)) / (1.0 + math.exp(max(x, -60.0)))


def frame_dbfs_zcr(frame: array.array) -> tuple[float, float]:
    """RMS level in dBFS and zero-crossing rate (crossings per sample) for one frame.

    One pass, no numpy. At 16 kHz / 512-sample frames this is ~31 calls per second of audio over
    512 integers — nothing that needs optimising, and a pure-stdlib implementation is the point.
    """
    n = len(frame)
    if n == 0:
        return _DBFS_FLOOR, 0.0
    total = 0
    crossings = 0
    prev_positive = frame[0] >= 0
    for x in frame:
        total += x * x
        positive = x >= 0
        if positive != prev_positive:
            crossings += 1
            prev_positive = positive
    mean_square = total / n
    if mean_square <= 0.0:
        dbfs = _DBFS_FLOOR
    else:
        dbfs = max(_DBFS_FLOOR, 10.0 * math.log10(mean_square / (_FULL_SCALE * _FULL_SCALE)))
    zcr = crossings / (n - 1) if n > 1 else 0.0
    return dbfs, zcr


class EnergyZcrScorer(FrameScorer):
    """Stdlib-only fallback: frame energy against an adaptive noise floor, plus zero-crossing rate.

    **It is worse than Silero, and these are the ways.** Stated here rather than discovered later:

    * It detects *sound*, not *speech*. A door, a keyboard, a cough and a loud fan step are all
      "speech" if they clear the SNR gate. Silero was trained to reject exactly those.
    * It needs a calibration window (`energy_warmup_ms`) and reports nothing during it. A stream
      that opens mid-word therefore loses that word's onset, and the noise floor is initialised
      from speech, which suppresses detection until the first real pause pulls the floor down.
    * Steady broadband noise that starts *after* calibration reads as speech until the slow
      upward adaptation catches up — of the order of a second.
    * Zero-crossing rate is a weak feature and is used only to bring unvoiced onsets (/s/, /f/)
      forward a frame or two, where energy alone is late. It cannot separate a fricative from
      white noise, and this implementation does not pretend it can.
    * It has no notion of the difference between speech and speech-shaped music or a television.

    The pseudo-probability it returns is a monotone squashing of SNR in dB — **not** a calibrated
    probability. It is scaled so that 0.5 falls exactly at `energy_snr_db`, which is what makes
    the same `threshold` setting mean something comparable across the two backends. It should
    never be reported as a confidence, and any bake-off number produced with this backend must
    say which backend produced it (plan.md §4: a backend must not silently receive a different
    task than the one it is being compared against).
    """

    name = "energy-zcr"

    def __init__(self, config: VadConfig) -> None:
        self._cfg = config
        self._warmup_frames = int(
            math.ceil(ms_to_samples(config.energy_warmup_ms, config.sample_rate) / config.frame_samples)
        )
        self.reset()

    def reset(self) -> None:
        self._noise_dbfs = self._cfg.energy_initial_noise_dbfs
        self._frames_seen = 0
        self._warmup_min: float | None = None

    @property
    def noise_floor_dbfs(self) -> float:
        """Exposed for diagnostics and tests. Not part of the `FrameScorer` interface."""
        return self._noise_dbfs

    def score(self, frame: array.array) -> float:
        cfg = self._cfg
        dbfs, zcr = frame_dbfs_zcr(frame)
        self._frames_seen += 1

        if self._frames_seen <= self._warmup_frames:
            # Calibrating. Take the *quietest* frame seen so far rather than the mean: the mean of
            # a window containing speech is dominated by the speech, and a floor set too high is
            # the failure that makes the detector deaf, which is worse than one set too low (that
            # one is caught by `energy_abs_floor_dbfs`).
            self._warmup_min = dbfs if self._warmup_min is None else min(self._warmup_min, dbfs)
            self._noise_dbfs = self._warmup_min
            return 0.0

        snr_db = dbfs - self._noise_dbfs
        if cfg.zcr_min <= zcr <= cfg.zcr_max:
            snr_db += cfg.zcr_boost_db
        prob = _logistic((snr_db - cfg.energy_snr_db) / cfg.energy_snr_slope_db)
        if dbfs < cfg.energy_abs_floor_dbfs:
            # Below the absolute floor nothing is speech, however good the SNR looks. This is what
            # stops a floor tracked down to digital silence from turning dither into dictation.
            prob = 0.0

        if dbfs < self._noise_dbfs:
            self._noise_dbfs += cfg.energy_noise_down * (dbfs - self._noise_dbfs)
        elif prob < cfg.threshold:
            self._noise_dbfs += cfg.energy_noise_up * (dbfs - self._noise_dbfs)
        return prob


class SileroOnnxScorer(FrameScorer):
    """Silero VAD through ONNX Runtime — the intended production path.

    `onnxruntime` and `numpy` are imported inside `load()`, never at module import, so a sidecar
    without them starts normally and loses this one backend (repository rule; plan.md §1.1).

    **Nothing here downloads a model.** plan.md §1.9 forbids model auto-download at runtime;
    fetching weights is an explicit, separate, user-initiated action. The path comes from
    `VadConfig.silero_model_path` and a missing file is reported with that exact path.

    ─── What is unverified here, and what this code does about it ─────────────────────────────

    No Silero model file was present while this was written, and no `onnxruntime` is installed in
    this repository's environments (nothing may be installed — repository rule), so **none of the
    following has been executed**:

    * *Graph signature.* Published Silero v5 exports are described as taking `input` (float32
      `[batch, samples]`), `sr` (int64 scalar) and a single recurrent `state` (`[2, batch, 128]`),
      while v4 took separate `h` and `c` (`[2, batch, 64]`). Rather than hard-coding either, this
      class **introspects `session.get_inputs()` at load time** and builds the feed dictionary
      from the names and declared shapes it actually finds. Symbolic dimensions are filled from a
      small table of known names, and anything it cannot make sense of raises
      `VadBackendUnavailable` with the observed signature in the message — a loud, diagnosable
      failure rather than a confident wrong guess.
    * *Output order.* The first output is taken as the speech probability and the remaining
      outputs are matched positionally to the recurrent inputs. The count is checked at load
      time; a mismatch raises rather than silently feeding stale state.
    * *Window size.* Silero's published 16 kHz usage requires exactly 512-sample windows. This is
      enforced (as a `VadConfigError`, because it is a configuration mistake, not an unavailable
      runtime) but has not been confirmed against a real model here.

    The first person to run this with a real `silero_vad.onnx` should confirm those three points
    and delete this paragraph down to whatever is left untrue.
    """

    name = "silero-onnx"

    # Fallback shapes for recurrent inputs whose declared dimensions are symbolic. Documented as
    # unverified above; used only when the model does not state a concrete shape.
    _KNOWN_STATE_SHAPES = {"state": (2, 1, 128), "h": (2, 1, 64), "c": (2, 1, 64)}
    _SAMPLE_INPUT_NAMES = ("input", "audio", "x")
    _RATE_INPUT_NAMES = ("sr", "sample_rate")

    def __init__(self, config: VadConfig) -> None:
        self._cfg = config
        self._session = None
        self._np = None
        self._sample_input = ""
        self._rate_input: str | None = None
        self._state_inputs: list[tuple[str, tuple[int, ...]]] = []
        self._state: dict[str, object] = {}
        self._output_names: list[str] = []

    # ── Loading ────────────────────────────────────────────────────────────────────────────────

    def load(self) -> None:
        cfg = self._cfg

        # Configuration faults first, and loudly: these are deployment mistakes and must not be
        # absorbed by `create_vad()`'s fallback.
        if cfg.sample_rate != 16000:
            raise VadConfigError(
                "Silero VAD is used here at 16 kHz only (the wire format is fixed at 16 kHz by "
                "docs/protocol.md §6.3); config says sample_rate=%d" % cfg.sample_rate
            )
        if cfg.frame_samples != 512:
            raise VadConfigError(
                "Silero VAD at 16 kHz requires 512-sample windows; config says frame_samples=%d. "
                "Set VadConfig.frame_samples=512 or select the energy backend." % cfg.frame_samples
            )

        # The path is checked before the runtime is imported. Both are prerequisites, but a
        # missing or mistyped path is by far the more common failure and its message is the more
        # actionable one, so it wins the race to be reported.
        path = cfg.silero_model_path
        if not path:
            raise VadBackendUnavailable(
                "no Silero VAD model configured (VadConfig.silero_model_path is unset). The model "
                "is not downloaded automatically (plan.md §1.9); point this at a local "
                "silero_vad.onnx."
            )
        path = os.path.abspath(os.path.expanduser(path))
        if not os.path.isfile(path):
            raise VadBackendUnavailable(
                "Silero VAD model not found at %r (resolved from %r). Nothing is downloaded "
                "automatically (plan.md §1.9); place silero_vad.onnx there or select the energy "
                "backend." % (path, cfg.silero_model_path)
            )

        try:
            import numpy  # noqa: F401  (kept as an attribute below)
            import onnxruntime
        except ImportError as exc:
            raise VadBackendUnavailable(
                "onnxruntime/numpy not importable in this interpreter (%s); the Silero backend is "
                "unavailable. See server/requirements for the pinned install." % exc
            ) from exc

        self._np = numpy

        options = onnxruntime.SessionOptions()
        # A VAD is a few hundred microseconds of work per 32 ms of audio. Threads bought nothing
        # and cost scheduler noise on a machine that is simultaneously running an ASR model.
        options.intra_op_num_threads = cfg.silero_intra_op_threads
        options.inter_op_num_threads = 1
        try:
            self._session = onnxruntime.InferenceSession(
                path, sess_options=options, providers=list(cfg.silero_providers)
            )
        except Exception as exc:  # onnxruntime raises its own exception types; keep it broad
            raise VadBackendUnavailable(
                "onnxruntime could not load %r: %s" % (path, exc)
            ) from exc

        self._bind_signature(path)
        self.reset()
        logger.info(
            "Silero VAD loaded from %s (inputs: sample=%r rate=%r state=%r, providers=%r)",
            path,
            self._sample_input,
            self._rate_input,
            [n for n, _ in self._state_inputs],
            getattr(self._session, "get_providers", lambda: cfg.silero_providers)(),
        )

    def _bind_signature(self, path: str) -> None:
        """Work out the graph's calling convention from the model itself.

        Written this way because the exact signature differs between published Silero versions and
        this repository has no model to check against. Introspection turns "which version is this
        file?" from a guess into an observation.
        """
        inputs = self._session.get_inputs()
        names = [i.name for i in inputs]

        sample_input = next((n for n in names if n in self._SAMPLE_INPUT_NAMES), None)
        if sample_input is None:
            raise VadBackendUnavailable(
                "%r does not look like a Silero VAD graph: no audio input among %r (expected one "
                "of %r)" % (path, names, self._SAMPLE_INPUT_NAMES)
            )
        self._sample_input = sample_input
        self._rate_input = next((n for n in names if n in self._RATE_INPUT_NAMES), None)

        state_inputs: list[tuple[str, tuple[int, ...]]] = []
        for spec in inputs:
            if spec.name == sample_input or spec.name == self._rate_input:
                continue
            shape = self._concrete_shape(spec)
            if shape is None:
                raise VadBackendUnavailable(
                    "%r: cannot determine a concrete shape for recurrent input %r (declared %r) "
                    "and its name is not one of the known Silero state inputs %r"
                    % (path, spec.name, spec.shape, tuple(self._KNOWN_STATE_SHAPES))
                )
            state_inputs.append((spec.name, shape))
        self._state_inputs = state_inputs

        self._output_names = [o.name for o in self._session.get_outputs()]
        # outputs = [probability, *new recurrent state]. Asserted rather than assumed, because a
        # mismatch would mean the state is never updated and every frame is scored as if it were
        # the first — which looks like a working VAD with mysteriously poor accuracy.
        if len(self._output_names) != 1 + len(state_inputs):
            raise VadBackendUnavailable(
                "%r: expected 1 probability output plus %d recurrent outputs, found %r"
                % (path, len(state_inputs), self._output_names)
            )

    def _concrete_shape(self, spec) -> tuple[int, ...] | None:
        """Prefer the model's declared dims; fill symbolic ones from the known-name table."""
        declared = list(getattr(spec, "shape", []) or [])
        known = self._KNOWN_STATE_SHAPES.get(spec.name)
        resolved: list[int] = []
        for index, dim in enumerate(declared):
            if isinstance(dim, int) and dim > 0:
                resolved.append(dim)
            elif known is not None and index < len(known):
                resolved.append(known[index])
            elif isinstance(dim, str) and ("batch" in dim.lower()):
                resolved.append(1)
            else:
                return None
        if not resolved:
            return known
        return tuple(resolved)

    # ── Scoring ────────────────────────────────────────────────────────────────────────────────

    def reset(self) -> None:
        if self._np is None:
            return
        self._state = {
            name: self._np.zeros(shape, dtype=self._np.float32)
            for name, shape in self._state_inputs
        }

    def score(self, frame: array.array) -> float:
        if self._session is None:
            raise VadBackendUnavailable("SileroOnnxScorer.load() has not been called")
        np = self._np
        # `<i2` is explicit rather than `np.int16`: the frame came from the wire as s16le and
        # `pcm_to_samples` normalised it to native order, so on a big-endian host a native-dtype
        # view would be right only by luck. Dividing by 32768 keeps the range in [-1, 1).
        pcm = samples_to_pcm(frame)
        audio = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / _FULL_SCALE
        feeds = {self._sample_input: audio.reshape(1, -1)}
        if self._rate_input is not None:
            feeds[self._rate_input] = np.array(self._cfg.sample_rate, dtype=np.int64)
        feeds.update(self._state)

        outputs = self._session.run(self._output_names, feeds)
        prob = float(np.asarray(outputs[0]).reshape(-1)[0])
        for (name, _shape), value in zip(self._state_inputs, outputs[1:]):
            self._state[name] = value
        # Clamp rather than trust: the endpointer's hysteresis assumes a bounded score, and a
        # model that ever returned something outside [0, 1] would make the thresholds meaningless.
        return min(1.0, max(0.0, prob))

    def close(self) -> None:
        self._session = None
        self._state = {}


# ─── The endpointer ─────────────────────────────────────────────────────────────────────────────


class VadEndpointer:
    """Turns a stream of frame scores into utterance boundaries and a gated decoder feed.

    Feed it audio with `process()`; tell it about a WASAPI glitch with `notify_discontinuity()`;
    close the stream with `flush()`. Every call returns a `VadChunk`.

    The state machine, in full:

        SILENCE ──(scores ≥ threshold for min_speech_ms)──▶ SPEECH        emits SPEECH_START
        SPEECH  ──(scores < neg_threshold for hangover_ms)─▶ SILENCE      emits SPEECH_END
        SILENCE ──(endpoint_silence_ms since last speech)──▶ (utterance closed)  emits UTTERANCE_END

    An utterance opens at the first SPEECH_START and survives every SPEECH_END until the endpoint
    timer expires — which is what makes a brief mid-sentence pause harmless and is the entire
    point of the module (plan.md §1.6).
    """

    def __init__(self, config: VadConfig | None = None, scorer: FrameScorer | None = None) -> None:
        self._cfg = config or VadConfig()
        self._scorer = scorer if scorer is not None else EnergyZcrScorer(self._cfg)

        n = self._cfg.frame_samples
        # Enough history to satisfy the largest pre-roll request: the pre-roll itself, plus the
        # min-speech window that elapses before an onset can be confirmed, plus slack.
        self._history_cap = self._cfg.preroll_samples + self._cfg.min_speech_samples + 2 * n

        self._buf = array.array("h")          # samples not yet forming a whole frame
        self._history = array.array("h")      # recent samples, for pre-roll
        self._history_start = 0               # stream position of _history[0]
        self._pos = 0                         # samples consumed into frames so far

        self._state = VadState.SILENCE
        self._utterance_open = False
        self._utterance_index = 0             # 1-based once the first utterance opens
        self._utterance_start = 0
        self._pending_start: int | None = None
        self._pending_samples = 0
        self._silence_samples = 0
        self._last_speech_end = 0
        self._last_boundary = 0               # position of the most recent utterance end
        self._last_prob = 0.0

    # ── Introspection ──────────────────────────────────────────────────────────────────────────

    @property
    def config(self) -> VadConfig:
        return self._cfg

    @property
    def backend_name(self) -> str:
        return self._scorer.name

    @property
    def state(self) -> VadState:
        return self._state

    @property
    def gate_open(self) -> bool:
        """The decoder-gating signal: True while audio should be fed to the decoder.

        Identical to "an utterance is open". It opens at speech onset (with pre-roll), stays open
        across a short mid-sentence pause, and closes at the endpoint — at which point the session
        loop must reset decoder state (plan.md §1.6).
        """
        return self._utterance_open

    @property
    def utterance_index(self) -> int:
        """1-based count of utterances seen on this stream; 0 before the first onset."""
        return self._utterance_index

    @property
    def position_samples(self) -> int:
        """Stream position, in samples, of the end of the last complete frame processed."""
        return self._pos

    @property
    def position_ms(self) -> float:
        return samples_to_ms(self._pos, self._cfg.sample_rate)

    @property
    def last_probability(self) -> float:
        return self._last_prob

    # ── Feeding ────────────────────────────────────────────────────────────────────────────────

    def process(self, pcm: bytes) -> VadChunk:
        """Consume s16le audio. Any length (even byte count); frames are reassembled internally.

        The caller's chunk size is irrelevant to the result: the same stream split differently
        produces identical events, which is asserted in the tests for the same reason
        docs/protocol.md §2.2 insists on a byte-stream framing decoder — a component that is only
        correct at one chunking is correct by accident.
        """
        samples = pcm_to_samples(pcm)
        self._buf.extend(samples)

        events: list[VadEvent] = []
        out = _SegmentBuilder()
        n = self._cfg.frame_samples
        while len(self._buf) >= n:
            frame = self._buf[:n]
            del self._buf[:n]
            self._on_frame(frame, events, out)
        return VadChunk(tuple(events), out.finish(), self._utterance_open)

    def flush(self) -> VadChunk:
        """End of stream: close any open utterance so nothing is left un-finalised.

        Called on `stop` (docs/protocol.md §6.1: "finalise what exists, then end"). Emits the same
        events an endpoint would, with `reason="flush"` so a log reader can tell a real endpoint
        from a forced one. Sub-frame residue is emitted to the decoder if the gate is open — it is
        under one frame of audio but it is the tail of a word, and dropping it would clip the last
        phoneme of the session.
        """
        events: list[VadEvent] = []
        out = _SegmentBuilder()
        if self._utterance_open and self._buf:
            out.add(self._utterance_index, self._pos, self._buf)
        residue = len(self._buf)
        self._pos += residue
        self._buf = array.array("h")
        self._close_utterance(events, reason="flush", end_sample=None)
        return VadChunk(tuple(events), out.finish(), self._utterance_open)

    def notify_discontinuity(self, gap_ms: int = 0, reason: str = "") -> VadChunk:
        """Handle docs/protocol.md §6.1 `discontinuity` — the audio stream is not contiguous.

        The C++ side sends this when WASAPI reports `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`. The
        two silent options — pad with zeroes, or splice — both hand the decoder a lie.

        What this does: **ends the open utterance and resets everything.** Ending the utterance is
        not optional dramatics; it follows from the protocol. Resetting decoder state mid-utterance
        would make the next `partial.text` for that `utterance_id` no longer a superset of the
        previous one, and docs/protocol.md §8.1 requires `partial.text` to be cumulative for the
        utterance while §8.2 forbids the stable prefix from mutating. So a decoder reset and an
        utterance boundary must happen together, or the wire contract is violated.

        `gap_ms` advances the position counter by the lost duration, so this module's sample
        positions stay aligned with the Client's audio timeline across the gap. `gap_ms == 0`
        means "duration unknown" (§6.1) and advances nothing — inventing a duration would put
        every subsequent position quietly out of step with the Client's.
        """
        events: list[VadEvent] = []
        gap_samples = ms_to_samples(gap_ms, self._cfg.sample_rate) if gap_ms > 0 else 0
        at = self._pos
        self._close_utterance(
            events, reason=reason or "discontinuity", end_sample=None, decided_at=at
        )
        self._pos += gap_samples
        events.append(
            VadEvent(
                kind=VadEventKind.DISCONTINUITY,
                sample=at,
                decided_at_sample=self._pos,
                utterance_index=self._utterance_index,
                sample_rate=self._cfg.sample_rate,
                reason=reason or ("gap_ms=%d" % gap_ms),
            )
        )
        self._reset_stream_state()
        logger.info(
            "audio discontinuity at sample %d (gap_ms=%d, reason=%r); VAD state reset and any "
            "open utterance closed",
            at,
            gap_ms,
            reason,
        )
        return VadChunk(tuple(events), (), self._utterance_open)

    def reset(self) -> None:
        """Full reset, discarding buffered audio and event state but keeping the position clock.

        Used between sessions. Not the same as `notify_discontinuity()`, which emits events
        because the session loop must react to it.
        """
        self._reset_stream_state()
        self._utterance_open = False
        self._utterance_index = 0

    def close(self) -> None:
        self._scorer.close()

    # ── Internals ──────────────────────────────────────────────────────────────────────────────

    def _reset_stream_state(self) -> None:
        self._buf = array.array("h")
        self._history = array.array("h")
        self._history_start = self._pos
        self._state = VadState.SILENCE
        self._pending_start = None
        self._pending_samples = 0
        self._silence_samples = 0
        self._last_speech_end = self._pos
        self._last_boundary = self._pos
        self._scorer.reset()

    def _emit(
        self,
        events: list[VadEvent],
        kind: VadEventKind,
        sample: int,
        decided_at: int,
        reason: str = "",
    ) -> None:
        events.append(
            VadEvent(
                kind=kind,
                sample=sample,
                decided_at_sample=decided_at,
                utterance_index=self._utterance_index,
                sample_rate=self._cfg.sample_rate,
                reason=reason,
            )
        )

    def _close_utterance(
        self,
        events: list[VadEvent],
        reason: str,
        end_sample: int | None,
        decided_at: int | None = None,
    ) -> None:
        """Emit SPEECH_END (if speech was in progress) then UTTERANCE_END, and shut the gate."""
        if not self._utterance_open:
            return
        at = self._pos if decided_at is None else decided_at
        end = self._last_speech_end if end_sample is None else end_sample
        if self._state is VadState.SPEECH:
            self._state = VadState.SILENCE
            self._emit(events, VadEventKind.SPEECH_END, end, at, reason)
        self._emit(events, VadEventKind.UTTERANCE_END, end, at, reason)
        self._utterance_open = False
        self._last_boundary = at
        self._pending_start = None
        self._pending_samples = 0

    def _on_frame(
        self, frame: array.array, events: list[VadEvent], out: _SegmentBuilder
    ) -> None:
        cfg = self._cfg
        n = len(frame)
        start = self._pos
        end = start + n
        self._pos = end

        self._history.extend(frame)
        excess = len(self._history) - self._history_cap
        if excess > 0:
            del self._history[:excess]
            self._history_start += excess

        # Safety valve first, so the frame that opens the replacement utterance is attributed to
        # it rather than to the one being cut. See VadConfig.max_utterance_ms.
        if (
            self._utterance_open
            and cfg.max_utterance_samples
            and start - self._utterance_start >= cfg.max_utterance_samples
        ):
            self._emit(events, VadEventKind.UTTERANCE_END, start, start, "max_utterance")
            self._utterance_open = False
            self._last_boundary = start
            logger.warning(
                "utterance %d exceeded max_utterance_ms=%.0f and was cut at sample %d; this is a "
                "safety valve, not an endpoint — check the VAD threshold or the input level",
                self._utterance_index,
                cfg.max_utterance_ms,
                start,
            )
            if self._state is VadState.SPEECH:
                # Mid-speech cut: open the replacement immediately so the decoder feed stays
                # continuous. Re-qualifying through min_speech_ms would punch a hole in it.
                self._open_utterance(events, onset=start, decided_at=start, reason="max_utterance")

        gate_was_open = self._utterance_open
        prob = self._scorer.score(frame)
        self._last_prob = prob
        onset_this_frame = False

        threshold = cfg.neg_threshold if self._state is VadState.SPEECH else cfg.threshold
        speaking = prob >= threshold

        if speaking:
            # Any speaking frame resets the endpoint timer, including one inside a run too short
            # to qualify as an onset. min_speech_ms suppresses spurious *onsets*, not the timer:
            # the alternative — a sub-minimum blip that does not reset it — lets the endpoint fire
            # in the middle of a word that has just started, splitting it across two utterances.
            # The cost of this choice is that impulsive noise can delay finalisation; the defence
            # against that is the probability threshold and `max_utterance_ms`, not the timer.
            self._silence_samples = 0
            if self._state is VadState.SPEECH:
                self._last_speech_end = end
            else:
                if self._pending_start is None:
                    self._pending_start = start
                    self._pending_samples = 0
                self._pending_samples += n
                if self._pending_samples >= cfg.min_speech_samples:
                    # Backdate the onset to where the run began — that is where speech actually
                    # started; the detector is merely late. Clamp to the last utterance boundary
                    # so an onset can never be reported before the end of the previous utterance
                    # (possible when a run straddles the endpoint moment).
                    onset = max(self._pending_start, self._last_boundary)
                    self._state = VadState.SPEECH
                    self._last_speech_end = end
                    self._pending_start = None
                    self._pending_samples = 0
                    self._open_utterance(events, onset=onset, decided_at=end, reason="")
                    onset_this_frame = True
        else:
            self._pending_start = None
            self._pending_samples = 0
            self._silence_samples += n
            if self._state is VadState.SPEECH and self._silence_samples >= cfg.hangover_samples:
                self._state = VadState.SILENCE
                self._emit(events, VadEventKind.SPEECH_END, self._last_speech_end, end)
            if (
                self._utterance_open
                and self._state is VadState.SILENCE
                and self._silence_samples >= cfg.endpoint_samples
            ):
                # plan.md §1.6's endpoint: this ends the UTTERANCE. The session is untouched and
                # this module has no way to touch it.
                self._emit(
                    events, VadEventKind.UTTERANCE_END, self._last_speech_end, end, "endpoint"
                )
                self._utterance_open = False
                self._last_boundary = end

        # Audio routing. Exactly one of these applies: an onset that opened the gate replays the
        # pre-roll and this frame together, otherwise an already-open gate forwards this frame —
        # including the frame on which the utterance ended, whose (silent) tail belongs to it.
        if onset_this_frame and not gate_was_open:
            first = max(self._utterance_start - cfg.preroll_samples, self._history_start)
            offset = first - self._history_start
            out.add(self._utterance_index, first, self._history[offset:])
        elif gate_was_open:
            out.add(self._utterance_index, start, frame)

    def _open_utterance(
        self, events: list[VadEvent], onset: int, decided_at: int, reason: str
    ) -> None:
        if not self._utterance_open:
            self._utterance_index += 1
            self._utterance_open = True
            self._utterance_start = onset
        self._emit(events, VadEventKind.SPEECH_START, onset, decided_at, reason)


# ─── Backend resolution ─────────────────────────────────────────────────────────────────────────


def create_vad(config: VadConfig | None = None) -> VadEndpointer:
    """Build a `VadEndpointer` with the best backend this environment can actually provide.

    * `backend="silero"` — Silero or an exception. For the bake-off, where a silent fallback would
      quietly change what is being measured (plan.md §4).
    * `backend="energy"` — the stdlib fallback, deliberately.
    * `backend="auto"` (default) — Silero if it loads, otherwise the fallback with **one clear
      warning line naming the reason**. A downgrade this significant must never be silent: every
      accuracy number taken afterwards depends on which backend answered.
    """
    cfg = config or VadConfig()

    if cfg.backend == "energy":
        return VadEndpointer(cfg, EnergyZcrScorer(cfg))

    scorer = SileroOnnxScorer(cfg)
    try:
        scorer.load()
    except VadBackendUnavailable as exc:
        if cfg.backend == "silero":
            raise
        logger.warning(
            "Silero VAD unavailable (%s). Falling back to the stdlib energy/ZCR endpointer, which "
            "detects sound rather than speech and is measurably worse — see EnergyZcrScorer's "
            "docstring. Endpointing will work; accuracy numbers taken with it are not comparable "
            "to Silero's.",
            exc,
        )
        return VadEndpointer(cfg, EnergyZcrScorer(cfg))
    return VadEndpointer(cfg, scorer)
