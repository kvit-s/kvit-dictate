#!/usr/bin/env python3
# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
# -*- coding: utf-8 -*-
"""Replay the dictation corpus through two transcription services and compare them.

This is **not** `run_matrix.py`. That file is the plan.md §4 bake-off: a pre-registered decision
rule over a recorded corpus with written references, and it still has no corpus to run against.
This file answers a smaller and more immediate question — *the service on port A is what ships;
is the service on port B better?* — using the only corpus that actually exists on this machine:
`src/corpus.h`'s capture of real sessions, at `H:\\kvit-corpus` by default.

What that corpus is and is not
──────────────────────────────
It is real speech, from the real microphone, through the real gate, in the real room, by the
person who uses the product. That is better than anything a prompt list can buy.

It has **no written reference**. Nobody typed out what was said. So this harness cannot report
WER-against-truth and does not pretend to: every number below is either reference-free
(latency, RTF, truncation, empty-output rate, hallucination over non-speech) or explicitly
*divergence* between the two services, which is symmetric and says who differs, not who is
right. The one thing that can settle "who is right" is a human reading the disagreements, so
the harness writes an adjudication file and stops there. A benchmark that dresses divergence up
as accuracy is worse than no benchmark, because it looks like a decision.

Where the clips come from, and why not from the event log
─────────────────────────────────────────────────────────
The obvious cut is `[previous final, this final]` using each `final` event's `audio_pos_ms`.
**It is wrong, measurably**, and the first version of this file did it: reproduction came out at
25 %. A `final`'s position is where the event was *emitted*, which trails where the speech
actually ended by however long the endpoint timer plus the HTTP round trips took — in one
measured session, utterance 2's final sat at 24 542 ms while its speech began at about
16 000 ms, four and a half seconds before utterance 1's final was even recorded. Cutting on
those numbers hands the model half a sentence.

So the clips are cut by `server/kvit_asr.vad.VadEndpointer` — the sidecar's own endpointer, run
over the recording with the operator's own `endpoint_silence_ms`. The segments it yields are, by
construction, the audio the backend would have been fed. Nothing is estimated and nothing is
re-tuned.

Two guards keep the replay honest
─────────────────────────────────
1. **The incumbent must reproduce itself.** Per session, the incumbent's answers for every
   segment are concatenated and scored against the concatenation of the `final` texts the
   shipping service actually committed. Segment boundaries need not line up one-to-one for this
   to be meaningful — the words do. If replaying a session through the same service does not
   return what the user received, the segmentation is wrong and every other number in the run is
   built on sand. That rate is printed first and a run below `--min-reproduction` is refused.
2. **The services are asked in both orders.** Clip N asks A first, clip N+1 asks B first, so a
   cold cache or a warm GPU cannot systematically favour whichever is always second.

Latency here is a property of a LAN and somebody's GPU queue, exactly as
`server/kvit_asr/backends/openai_http.py` says, and it must not be quoted as a model's latency.
It is still the number that decides whether a service is usable for dictation, so it is
measured — labelled as service latency, which is what it is.

Stdlib only, CPython 3.12.

Usage
─────
    py -3.12 bench/ab_service.py run \\
        --a qwen=http://strixhalo.lan:10301,model=Qwen/Qwen3-ASR-1.7B \\
        --b r2t2=http://192.168.138.186:10311,model=netease-youdao/Confucius4-R2T2 \\
        --sample 150 --seed 7 --out bench/results/ab/<name>

    py -3.12 bench/ab_service.py report --out bench/results/ab/<name>
"""

from __future__ import annotations

import argparse
import io
import json
import os
import random
import re
import statistics
import sys
import time
import urllib.error
import urllib.request
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator, Optional, Sequence

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parent
sys.path.insert(0, str(BENCH_DIR))
sys.path.insert(0, str(REPO_ROOT / "server"))

import metrics  # noqa: E402  - after the path insert, deliberately
from kvit_asr.vad import VadConfig, VadEndpointer  # noqa: E402  - same

SCHEMA_VERSION = 1
HARNESS_NAME = "kvit-dictate/bench/ab_service.py"

DEFAULT_CORPUS = Path(r"H:\kvit-corpus")
SAMPLE_RATE = 16000
BYTES_PER_SAMPLE = 2

_PATH = "/v1/audio/transcriptions"


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# A service under test
# ═══════════════════════════════════════════════════════════════════════════════════════════════

@dataclass(frozen=True)
class Service:
    """One OpenAI-compatible `/v1/audio/transcriptions` endpoint.

    `name` is the key every result is filed under, so it must be stable across runs: renaming a
    service silently makes two runs incomparable.
    """

    name: str
    base_url: str
    model: Optional[str] = None
    language: Optional[str] = "en"
    timeout_s: float = 180.0
    #: Vocabulary hints, sent as the `prompt` form field. Both services on this LAN honour
    #: `prompt` and `context` identically and ignore `hotwords` and `initial_prompt`, measured
    #: 2026-09-20. The field exists here so "does the vocabulary the user maintains actually buy
    #: anything" is a question the harness can answer rather than one settled by one example.
    prompt: Optional[str] = None

    @staticmethod
    def parse(spec: str) -> "Service":
        """`name=url[,model=...][,language=...][,prompt=...]`. The url may not contain a comma,
        which no URL we care about does; rejecting it here beats a shell-quoting puzzle later.
        `prompt` may not either, so a term list is given as `prompt@file` instead."""
        name, _, rest = spec.partition("=")
        if not name or not rest:
            raise SystemExit(f"service spec must be name=url[,k=v...]; got {spec!r}")
        parts = rest.split(",")
        url = parts[0].rstrip("/")
        kw: dict[str, str] = {}
        for p in parts[1:]:
            k, _, v = p.partition("=")
            kw[k.strip()] = v.strip()
        lang = kw.get("language", "en")
        prompt = kw.get("prompt") or None
        if "prompt@" in kw:
            prompt = Path(kw["prompt@"]).read_text(encoding="utf-8").strip() or None
        return Service(
            name=name.strip(),
            base_url=url,
            model=kw.get("model") or None,
            language=(lang or None) if lang.lower() != "none" else None,
            timeout_s=float(kw.get("timeout_s", 180.0)),
            prompt=prompt,
        )

    def to_dict(self) -> dict:
        return {"name": self.name, "base_url": self.base_url, "model": self.model,
                "language": self.language, "prompt": self.prompt}


_BOUNDARY = "----kvit-dictate-ab-Xq7Nr2"


def _multipart(fields: dict, filename: str, payload: bytes) -> tuple[bytes, str]:
    """Same hand-rolled encoder as `openai_http.py`, and for the same reason: `email.mime`
    rewrites line endings, and a silently re-wrapped body is a bad thing to debug against
    somebody else's server."""
    out = bytearray()
    for key, value in fields.items():
        if value is None:
            continue
        out += f"--{_BOUNDARY}\r\n".encode("ascii")
        out += f'Content-Disposition: form-data; name="{key}"\r\n\r\n'.encode("ascii")
        out += str(value).encode("utf-8")
        out += b"\r\n"
    out += f"--{_BOUNDARY}\r\n".encode("ascii")
    out += (
        f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
        f"Content-Type: audio/wav\r\n\r\n"
    ).encode("ascii")
    out += payload
    out += b"\r\n"
    out += f"--{_BOUNDARY}--\r\n".encode("ascii")
    return bytes(out), f"multipart/form-data; boundary={_BOUNDARY}"


def wav_bytes(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(BYTES_PER_SAMPLE)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return buf.getvalue()


@dataclass
class Answer:
    text: str
    ms: float
    error: Optional[str] = None


def transcribe(svc: Service, pcm: bytes) -> Answer:
    body, content_type = _multipart(
        {"model": svc.model, "language": svc.language, "response_format": "json",
         "prompt": svc.prompt},
        "utterance.wav",
        wav_bytes(pcm),
    )
    req = urllib.request.Request(svc.base_url + _PATH, data=body, method="POST")
    req.add_header("Content-Type", content_type)
    req.add_header("Content-Length", str(len(body)))
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=svc.timeout_s) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            detail = exc.read().decode("utf-8", "replace")[:300]
        except Exception:  # noqa: BLE001 - best-effort context, never required
            pass
        return Answer("", (time.perf_counter() - t0) * 1000.0, f"HTTP {exc.code}: {detail}")
    except (urllib.error.URLError, OSError) as exc:
        return Answer("", (time.perf_counter() - t0) * 1000.0,
                      f"{type(exc).__name__}: {exc}")
    ms = (time.perf_counter() - t0) * 1000.0
    try:
        payload = json.loads(raw.decode("utf-8"))
        text = payload["text"]
        if not isinstance(text, str):
            raise TypeError("text is not a string")
    except Exception as exc:  # noqa: BLE001 - one case: the answer was not usable
        return Answer("", ms, f"bad response ({type(exc).__name__}: {exc})")
    return Answer(text.strip(), ms)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# The corpus: sessions in, utterance clips out
# ═══════════════════════════════════════════════════════════════════════════════════════════════

@dataclass
class Utterance:
    """One utterance the sidecar's endpointer carved out of a session recording.

    `pcm` is held rather than re-read: it is exactly the bytes `VadEndpointer` gated through, and
    re-deriving them from a byte offset would reintroduce the ambiguity the endpointer just
    removed.
    """

    session: str
    utt: int
    start_ms: int
    end_ms: int
    audio: bytes
    kind: str = "speech"

    @property
    def key(self) -> str:
        return f"{self.session}-u{self.utt}"

    @property
    def duration_ms(self) -> int:
        return int(len(self.audio) / BYTES_PER_SAMPLE * 1000 / SAMPLE_RATE)

    def pcm(self) -> bytes:
        return self.audio


def read_session_audio(audio_path: Path) -> bytes:
    with wave.open(str(audio_path), "rb") as w:
        if w.getframerate() != SAMPLE_RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise ValueError(
                f"{audio_path} is not 16 kHz mono s16 "
                f"({w.getframerate()} Hz, {w.getnchannels()} ch, {w.getsampwidth()*8} bit)"
            )
        return w.readframes(w.getnframes())


def segment_session(session: str, pcm: bytes, *, endpoint_silence_ms: float) -> list[Utterance]:
    """Run the sidecar's endpointer over a whole recording and collect its utterances.

    `backend="energy"` rather than `"auto"`: the Silero ONNX scorer is not installed here and
    `auto` would fall back silently, which would make a run's segmentation depend on what happens
    to be importable on the machine. A measurement that changes with the environment and does not
    say so is the failure this whole directory is arranged against.
    """
    cfg = VadConfig(backend="energy", endpoint_silence_ms=endpoint_silence_ms)
    vad = VadEndpointer(cfg)
    by_utt: dict[int, list[bytes]] = {}
    starts: dict[int, int] = {}
    ends: dict[int, int] = {}
    step = SAMPLE_RATE * BYTES_PER_SAMPLE // 10  # 100 ms, the protocol's recommended feed size
    for off in range(0, len(pcm), step):
        for seg in vad.process(pcm[off:off + step]).segments:
            by_utt.setdefault(seg.utterance_index, []).append(seg.pcm)
            starts.setdefault(seg.utterance_index, seg.start_sample)
            ends[seg.utterance_index] = seg.end_sample
    for seg in vad.flush().segments:
        by_utt.setdefault(seg.utterance_index, []).append(seg.pcm)
        starts.setdefault(seg.utterance_index, seg.start_sample)
        ends[seg.utterance_index] = seg.end_sample
    return [
        Utterance(
            session=session,
            utt=idx,
            start_ms=int(starts[idx] * 1000 / SAMPLE_RATE),
            end_ms=int(ends[idx] * 1000 / SAMPLE_RATE),
            audio=b"".join(chunks),
        )
        for idx, chunks in sorted(by_utt.items())
    ]


def shipped_text(events_path: Path) -> str:
    """Everything the live service committed in this session, in order, joined by a space.

    Utterance-by-utterance alignment is deliberately not attempted: the event log's positions
    trail the audio (see the module docstring), so the only sound comparison is at session
    level, over the words."""
    out: list[str] = []
    with events_path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if obj.get("t") == "final":
                t = str(obj.get("text", "")).strip()
                if t:
                    out.append(t)
    return " ".join(out)


def nonspeech_clips(session: str, pcm: bytes, utts: Sequence[Utterance], *,
                    min_ms: int, max_ms: int) -> list[Utterance]:
    """The audio between utterances: the microphone was open and the endpointer said no.

    A better hallucination probe than synthetic silence, for the reason `prompts/ambient.jsonl`
    gives — models invent words over a real noise floor, not over a digital zero. The reference
    is empty by construction, so `metrics.is_hallucination` applies directly.
    """
    total_ms = len(pcm) * 1000 // (SAMPLE_RATE * BYTES_PER_SAMPLE)
    spoken = sorted((u.start_ms, u.end_ms) for u in utts)
    gaps: list[tuple[int, int]] = []
    cursor = 0
    for start_ms, end_ms in spoken:
        if start_ms - cursor >= min_ms:
            gaps.append((cursor, start_ms))
        cursor = max(cursor, end_ms)
    if total_ms - cursor >= min_ms:
        gaps.append((cursor, total_ms))

    out: list[Utterance] = []
    for i, (a_ms, b_ms) in enumerate(gaps):
        b_ms = min(b_ms, a_ms + max_ms)
        a = a_ms * SAMPLE_RATE * BYTES_PER_SAMPLE // 1000
        b = b_ms * SAMPLE_RATE * BYTES_PER_SAMPLE // 1000
        out.append(Utterance(session=session, utt=-(i + 1), start_ms=a_ms, end_ms=b_ms,
                             audio=pcm[a:b], kind="nonspeech"))
    return out


def _session_header(events_path: Path) -> dict:
    with events_path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if obj.get("t") == "start":
                return obj
            return {}
    return {}


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# Service-shaped observations that are not accuracy
# ═══════════════════════════════════════════════════════════════════════════════════════════════

#: Confucius4-R2T2 emits its Longest-Stable-Prefix boundary as a literal `|` and, when the
#: wrapper does not flush the unstable tail, the text simply stops there. That is a property of
#: how the service is deployed rather than of the model, so it is counted separately and never
#: folded into an error rate: a truncated answer is not a mis-recognised one, it is a missing
#: one, and the fix is a `finish` call on the far side, not a better model.
TRUNCATION_MARK = "|"


def is_truncated(text: str) -> bool:
    return text.rstrip().endswith(TRUNCATION_MARK)


def strip_mark(text: str) -> str:
    return text.rstrip().removesuffix(TRUNCATION_MARK).rstrip()


_WORD = re.compile(r"\S+")


def word_count(text: str) -> int:
    return len(_WORD.findall(text))


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# The run
# ═══════════════════════════════════════════════════════════════════════════════════════════════

#: Everything this harness writes except the aggregate carries verbatim text from the operator's
#: own dictation — `clips.jsonl`, `sessions.jsonl`, `adjudicate.md` and the extracted clip WAVs.
#: The repository's `.gitignore` denies `bench/results/**` but then re-admits `*.md`, which would
#: commit the adjudication file and with it whatever the user happened to dictate that fortnight.
#: Rather than rely on whoever adds the next exception remembering this, the output directory is
#: given its own `.gitignore` that excludes everything but the aggregate. plan.md §1.9 is a rule
#: about data, and a rule about data that depends on being remembered is not one.
_OUTPUT_GITIGNORE = """\
# Written by bench/ab_service.py. Everything here but the aggregate contains verbatim
# transcripts of real dictation (plan.md 1.9). Do not add exceptions to this file.
*
!.gitignore
!*.aggregate.json
!*.aggregate.csv
"""


def protect_output_dir(out_dir: Path) -> None:
    (out_dir / ".gitignore").write_text(_OUTPUT_GITIGNORE, encoding="utf-8")


#: Keys whose presence means a structure carries corpus text. Mirrors
#: `run_matrix.TRANSCRIPT_KEYS`; duplicated rather than imported because `run_matrix` pulls in
#: the whole experiment grid and this file must stay runnable on its own.
TRANSCRIPT_KEYS = frozenset({
    "text", "texts", "hypothesis", "hypotheses", "reference", "references", "shipped",
    "answers", "observations", "commit_trace", "hallucination_texts", "adjudication",
})


def assert_no_verbatim(value: Any, path: str = "summary") -> None:
    """Refuse to write an aggregate that carries transcript text. The writer is right."""
    if isinstance(value, dict):
        for k, v in value.items():
            if k in TRANSCRIPT_KEYS:
                raise AssertionError(
                    f"{path}.{k} is a transcript-bearing key; aggregates may not contain corpus "
                    f"text (plan.md 1.9). Move it to a gitignored per-clip file."
                )
            assert_no_verbatim(v, f"{path}.{k}")
    elif isinstance(value, (list, tuple)):
        for i, v in enumerate(value):
            assert_no_verbatim(v, f"{path}[{i}]")


def sample_sessions(corpus_root: Path, *, n: int, seed: int,
                    service_filter: Optional[str]) -> tuple[list[Path], dict]:
    """Sessions, not utterances, are the sampling unit.

    Two reasons. Guard 1 is a session-level comparison and cannot be computed from a scattering
    of utterances taken out of different recordings. And utterances within one session are not
    independent — same room, same microphone gain, same topic — so a uniform draw over
    utterances would report a confidence the data does not have.

    plan.md §4: *any coverage cap is logged*. `caps` is that log.
    """
    every = sorted(p for p in corpus_root.iterdir()
                   if p.is_dir() and (p / "audio.wav").is_file() and (p / "events.jsonl").is_file())
    eligible = []
    skipped_service = 0
    for sess in every:
        header = _session_header(sess / "events.jsonl")
        if service_filter and header.get("service") and header["service"] != service_filter:
            skipped_service += 1
            continue
        eligible.append(sess)
    caps = {
        "sessions_in_corpus": len(every),
        "skipped_wrong_service": skipped_service,
        "service_filter": service_filter,
    }
    rng = random.Random(seed)
    if n and n < len(eligible):
        picked = sorted(rng.sample(eligible, n))
        caps["sessions_sampled"] = n
        caps["sampling"] = f"uniform over sessions without replacement, seed={seed}"
    else:
        picked = eligible
        caps["sessions_sampled"] = len(picked)
        caps["sampling"] = "every eligible session"
    return picked, caps


def run(args: argparse.Namespace) -> int:
    services = [Service.parse(args.a), Service.parse(args.b)]
    if services[0].name == services[1].name:
        raise SystemExit("the two services need different names; results are filed under them")
    corpus_root = Path(args.corpus)
    if not corpus_root.is_dir():
        raise SystemExit(f"no corpus at {corpus_root}")
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    protect_output_dir(out_dir)

    sessions, caps = sample_sessions(
        corpus_root, n=args.sessions, seed=args.seed, service_filter=args.service_filter)
    caps["duration_window_ms"] = [args.min_ms, args.max_ms]
    caps["endpoint_silence_ms"] = args.endpoint_silence_ms
    caps["nonspeech_min_ms"] = args.nonspeech_min_ms
    print(f"{len(sessions)} sessions; segmenting with the sidecar's endpointer "
          f"(energy backend, endpoint_silence_ms={args.endpoint_silence_ms:.0f})")

    rows_path = out_dir / "clips.jsonl"
    shipped_path = out_dir / "sessions.jsonl"
    started = time.strftime("%Y-%m-%dT%H:%M:%S")
    n_err = 0
    n_speech = n_noise = n_dropped = 0
    i = 0
    with rows_path.open("w", encoding="utf-8") as out, \
            shipped_path.open("w", encoding="utf-8") as sout:
        for s_i, sess in enumerate(sessions):
            try:
                pcm = read_session_audio(sess / "audio.wav")
            except Exception as exc:  # noqa: BLE001 - a bad recording must not end the run
                print(f"  ! {sess.name}: {exc}")
                continue
            utts = segment_session(sess.name, pcm,
                                   endpoint_silence_ms=args.endpoint_silence_ms)
            kept = [u for u in utts if args.min_ms <= u.duration_ms <= args.max_ms]
            n_dropped += len(utts) - len(kept)
            noise = nonspeech_clips(sess.name, pcm, utts,
                                    min_ms=args.nonspeech_min_ms, max_ms=args.nonspeech_max_ms)
            if args.nonspeech_per_session >= 0:
                noise = noise[:args.nonspeech_per_session]
            sout.write(json.dumps({
                "session": sess.name,
                "shipped": shipped_text(sess / "events.jsonl"),
                "segments": len(utts),
                "segments_kept": len(kept),
            }, ensure_ascii=False) + "\n")

            for clip in kept + noise:
                # Alternate who goes first, so neither service is systematically second.
                order = services if i % 2 == 0 else list(reversed(services))
                answers: dict[str, Answer] = {}
                for svc in order:
                    answers[svc.name] = transcribe(svc, clip.pcm())
                for a in answers.values():
                    if a.error:
                        n_err += 1
                out.write(json.dumps({
                    "session": clip.session,
                    "utt": clip.utt,
                    "kind": clip.kind,
                    "start_ms": clip.start_ms,
                    "end_ms": clip.end_ms,
                    "duration_ms": clip.duration_ms,
                    "first_asked": order[0].name,
                    "answers": {
                        name: {"text": a.text, "ms": round(a.ms, 1), "error": a.error}
                        for name, a in answers.items()
                    },
                }, ensure_ascii=False) + "\n")
                i += 1
                if clip.kind == "speech":
                    n_speech += 1
                else:
                    n_noise += 1
            print(f"  [{s_i+1}/{len(sessions)}] {sess.name}: "
                  f"{len(kept)} speech + {len(noise)} non-speech  ({i} clips so far)")

    caps["speech_clips"] = n_speech
    caps["nonspeech_clips"] = n_noise
    caps["dropped_outside_duration_window"] = n_dropped

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "harness": HARNESS_NAME,
        "metrics_versions": metrics.metric_versions(),
        "started": started,
        "finished": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "corpus_root": str(corpus_root),
        "services": [s.to_dict() for s in services],
        "incumbent": services[0].name,
        "coverage_caps": caps,
        "request_errors": n_err,
        "note": (
            "The corpus has no written reference. Nothing in this run is WER-against-truth. "
            "Cross-service numbers are DIVERGENCE and are symmetric."
        ),
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"wrote {rows_path}")
    if n_err:
        print(f"WARNING: {n_err} requests errored; see clips.jsonl")
    return report(argparse.Namespace(out=args.out, min_reproduction=args.min_reproduction,
                                     adjudicate=args.adjudicate))


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# The report
# ═══════════════════════════════════════════════════════════════════════════════════════════════

def _pct(x: float) -> str:
    return f"{100.0*x:.1f}%"


def _spread(values: Sequence[float], nd: int = 1) -> dict:
    if not values:
        return {"n": 0}
    s = sorted(values)
    def q(p: float) -> float:
        if len(s) == 1:
            return s[0]
        idx = p * (len(s) - 1)
        lo, hi = int(idx), min(int(idx) + 1, len(s) - 1)
        return s[lo] + (s[hi] - s[lo]) * (idx - lo)
    return {"n": len(s), "mean": round(statistics.fmean(s), nd), "p50": round(q(0.5), nd),
            "p90": round(q(0.9), nd), "p99": round(q(0.99), nd), "max": round(s[-1], nd)}


def report(args: argparse.Namespace) -> int:
    out_dir = Path(args.out)
    manifest = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
    rows = [json.loads(l) for l in (out_dir / "clips.jsonl").read_text(encoding="utf-8").splitlines() if l.strip()]
    names = [s["name"] for s in manifest["services"]]
    incumbent = manifest["incumbent"]
    challenger = next(n for n in names if n != incumbent)

    speech = [r for r in rows if r["kind"] == "speech"]
    nonspeech = [r for r in rows if r["kind"] == "nonspeech"]

    # ── Guard 1: does the replay reproduce what the user actually received? ───────────────────
    #
    # Session level, over words: concatenate the incumbent's answers for every speech clip of a
    # session and score them against the concatenation of that session's `final` texts. This is
    # a *word* comparison on purpose — the endpointer here need not split a session in the same
    # places the live one did, and it should not have to, because a re-segmentation that says
    # the same words is not a defect.
    sessions_meta = {
        row["session"]: row
        for row in (json.loads(l) for l in
                    (out_dir / "sessions.jsonl").read_text(encoding="utf-8").splitlines() if l.strip())
    }
    per_session_repro: list[tuple[str, float]] = []
    repro_items = []
    for sess_name, meta in sessions_meta.items():
        ours = " ".join(r["answers"][incumbent]["text"] for r in speech
                        if r["session"] == sess_name and not r["answers"][incumbent]["error"])
        theirs = meta.get("shipped", "")
        if not theirs.strip():
            continue
        er = metrics.error_rates(theirs, ours)
        repro_items.append(er)
        per_session_repro.append((sess_name, er.wer_normalised.rate))
    repro_rates = metrics.aggregate_error_rates(repro_items) if repro_items else None
    # "Reproduction" is 1 - normalised word error against what shipped. Normalised rather than
    # raw because the live text went through the stability engine's prefix commits and this one
    # did not, so punctuation and case differ for reasons that are not recognition.
    repro_frac = (1.0 - repro_rates.wer_normalised.rate) if repro_rates else 0.0
    repro_exact = sum(1 for _, rate in per_session_repro if rate == 0.0)

    per: dict[str, dict] = {}
    for name in names:
        texts = [r["answers"][name] for r in speech]
        lat = [a["ms"] for a in texts if not a["error"]]
        rtf = [a["ms"] / r["duration_ms"] for r, a in zip(speech, texts)
               if not a["error"] and r["duration_ms"] > 0]
        per[name] = {
            "errors": sum(1 for a in texts if a["error"]),
            "empty": sum(1 for a in texts if not a["error"] and not a["text"].strip()),
            "truncated": sum(1 for a in texts if is_truncated(a["text"])),
            "latency_ms": _spread(lat),
            "rtf": _spread(rtf, nd=4),
            "words": sum(word_count(a["text"]) for a in texts),
        }
        if nonspeech:
            ns = [r["answers"][name] for r in nonspeech]
            hall = [a["text"] for a in ns if metrics.is_hallucination("", a["text"])]
            per[name]["nonspeech"] = {
                "clips": len(ns),
                "hallucinated": len(hall),
                "rate": round(len(hall) / len(ns), 4),
            }

    # ── Divergence, in both directions, and with truncation held separate ─────────────────────
    pairs = [(r["answers"][incumbent]["text"], r["answers"][challenger]["text"])
             for r in speech
             if not r["answers"][incumbent]["error"] and not r["answers"][challenger]["error"]]
    untrunc = [(a, b) for a, b in pairs if not is_truncated(b) and not is_truncated(a)]

    def diverge(ps: Sequence[tuple[str, str]]) -> dict:
        if not ps:
            return {"n": 0}
        agg = metrics.aggregate_error_rates(metrics.error_rates(a, b) for a, b in ps)
        d = agg.to_dict()
        return {
            "n": len(ps),
            "identical": sum(1 for a, b in ps if a == b),
            "identical_normalised": sum(1 for a, b in ps
                                        if metrics.normalise_text(a) == metrics.normalise_text(b)),
            "wer_raw": round(d["wer_raw"]["rate"], 4),
            "wer_normalised": round(d["wer_normalised"]["rate"], 4),
            "cer_raw": round(d["cer_raw"]["rate"], 4),
            "cer_normalised": round(d["cer_normalised"]["rate"], 4),
        }

    # ── Truncation cost: how much text does the challenger simply not return? ─────────────────
    trunc_rows = [r for r in speech if is_truncated(r["answers"][challenger]["text"])]
    trunc_lost = []
    for r in trunc_rows:
        inc_w = word_count(r["answers"][incumbent]["text"])
        chl_w = word_count(strip_mark(r["answers"][challenger]["text"]))
        trunc_lost.append(inc_w - chl_w)

    summary = {
        "schema_version": SCHEMA_VERSION,
        "harness": HARNESS_NAME,
        "manifest": manifest,
        "clips": {"speech": len(speech), "nonspeech": len(nonspeech)},
        "reproduction": {
            "what": f"per session: the incumbent ({incumbent}) replayed over the re-segmented "
                    f"recording, scored against the text it committed live",
            "sessions": len(per_session_repro),
            "sessions_word_identical": repro_exact,
            "rate": round(repro_frac, 4),
            "wer_normalised_vs_shipped": round(repro_rates.wer_normalised.rate, 4) if repro_rates else None,
            "wer_raw_vs_shipped": round(repro_rates.wer_raw.rate, 4) if repro_rates else None,
            "worst_sessions": sorted(per_session_repro, key=lambda kv: -kv[1])[:5],
        },
        "per_service": per,
        "divergence_all": diverge(pairs),
        "divergence_excluding_truncated": diverge(untrunc),
        "truncation": {
            "service": challenger,
            "clips": len(trunc_rows),
            "rate": round(len(trunc_rows) / len(speech), 4) if speech else 0.0,
            "words_lost_vs_incumbent": _spread([float(x) for x in trunc_lost]),
            "note": "a truncated answer is a MISSING one, not a wrong one; the fix is a flush "
                    "on the service side, not a different model",
        },
    }
    protect_output_dir(out_dir)
    assert_no_verbatim(summary)
    (out_dir / "summary.aggregate.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")

    # ── Adjudication set: the only thing that can turn divergence into accuracy ────────────────
    if args.adjudicate:
        disagreements = [
            r for r in speech
            if metrics.normalise_text(r["answers"][incumbent]["text"])
            != metrics.normalise_text(strip_mark(r["answers"][challenger]["text"]))
        ]
        disagreements.sort(
            key=lambda r: -metrics.error_rates(
                r["answers"][incumbent]["text"], r["answers"][challenger]["text"]
            ).wer_raw.rate
        )
        adj = out_dir / "adjudicate.md"
        with adj.open("w", encoding="utf-8") as fh:
            fh.write("# Adjudication set\n\n")
            fh.write(f"{len(disagreements)} of {len(speech)} clips where `{incumbent}` and "
                     f"`{challenger}` disagree after normalisation, worst first.\n\n")
            fh.write("Neither column is a reference. Write the correct text under **truth:** "
                     "for as many as you have patience for; `score_adjudicated.py` is not "
                     "written yet and this file is its input.\n\n---\n\n")
            clip_dir = out_dir / "clips"
            corpus_root = Path(manifest["corpus_root"])
            for r in disagreements[:args.adjudicate]:
                name = f"{r['session']}-u{r['utt']}"
                fh.write(f"## {name}  ({r['duration_ms']} ms)\n\n")
                # The clip is written out as well as located: adjudicating means listening, and
                # "seek to 16 342 ms in a four-minute recording" is a task nobody completes forty
                # times.
                wav_rel = f"clips/{name}.wav"
                try:
                    src = corpus_root / r["session"] / "audio.wav"
                    pcm = read_session_audio(src)
                    a = r["start_ms"] * SAMPLE_RATE * BYTES_PER_SAMPLE // 1000
                    b = r["end_ms"] * SAMPLE_RATE * BYTES_PER_SAMPLE // 1000
                    clip_dir.mkdir(parents=True, exist_ok=True)
                    (clip_dir / f"{name}.wav").write_bytes(wav_bytes(pcm[a:b]))
                    fh.write(f"- audio: [`{wav_rel}`]({wav_rel})  "
                             f"(`{r['session']}/audio.wav` [{r['start_ms']}..{r['end_ms']} ms])\n")
                except Exception as exc:  # noqa: BLE001 - a missing recording is not fatal here
                    fh.write(f"- audio: `{r['session']}/audio.wav` "
                             f"[{r['start_ms']}..{r['end_ms']} ms] (not extracted: {exc})\n")
                fh.write(f"- **{incumbent}:** {r['answers'][incumbent]['text']}\n")
                fh.write(f"- **{challenger}:** {r['answers'][challenger]['text']}\n")
                fh.write("- **truth:** \n\n")
        print(f"wrote {adj} ({min(len(disagreements), args.adjudicate)} of "
              f"{len(disagreements)} disagreements)")

    # ── Print ─────────────────────────────────────────────────────────────────────────────────
    print()
    print(f"REPRODUCTION  {incumbent} replayed vs what it committed live, over "
          f"{len(per_session_repro)} sessions: {_pct(repro_frac)} of words agree "
          f"(normalised word error {_pct(summary['reproduction']['wer_normalised_vs_shipped'] or 0)}, "
          f"raw {_pct(summary['reproduction']['wer_raw_vs_shipped'] or 0)}); "
          f"{repro_exact} sessions word-identical")
    if repro_frac < args.min_reproduction:
        print(f"  REFUSED: below --min-reproduction {args.min_reproduction}. The segmentation "
              f"does not reproduce the live run, so nothing below is trustworthy.")
        print("  worst sessions: " + ", ".join(
            f"{n} {_pct(r)}" for n, r in summary["reproduction"]["worst_sessions"]))
        return 2
    print()
    hdr = f"{'':<34}" + "".join(f"{n:>22}" for n in names)
    print(hdr)
    def line(label: str, fn) -> None:
        print(f"{label:<34}" + "".join(f"{fn(n):>22}" for n in names))
    line("request errors", lambda n: per[n]["errors"])
    line("empty answers", lambda n: per[n]["empty"])
    line("truncated (trailing '|')", lambda n: f"{per[n]['truncated']} "
                                               f"({_pct(per[n]['truncated']/max(1,len(speech)))})")
    line("latency mean ms", lambda n: per[n]["latency_ms"].get("mean", "-"))
    line("latency p90 ms", lambda n: per[n]["latency_ms"].get("p90", "-"))
    line("latency p99 ms", lambda n: per[n]["latency_ms"].get("p99", "-"))
    line("RTF mean", lambda n: per[n]["rtf"].get("mean", "-"))
    line("words returned", lambda n: per[n]["words"])
    if nonspeech:
        line("non-speech hallucination", lambda n: f"{per[n]['nonspeech']['hallucinated']}"
                                                   f"/{per[n]['nonspeech']['clips']}")
    print()
    d = summary["divergence_all"]
    du = summary["divergence_excluding_truncated"]
    print(f"DIVERGENCE (symmetric; NOT accuracy)   n={d['n']}")
    print(f"  identical verbatim            {d['identical']} ({_pct(d['identical']/max(1,d['n']))})")
    print(f"  identical after normalising   {d['identical_normalised']} "
          f"({_pct(d['identical_normalised']/max(1,d['n']))})")
    print(f"  word divergence  raw {_pct(d['wer_raw'])}   normalised {_pct(d['wer_normalised'])}")
    print(f"  excluding truncated clips (n={du['n']}): "
          f"raw {_pct(du.get('wer_raw', 0))}   normalised {_pct(du.get('wer_normalised', 0))}")
    t = summary["truncation"]
    print()
    print(f"TRUNCATION  {t['service']}: {t['clips']}/{len(speech)} clips ({_pct(t['rate'])}), "
          f"words missing vs {incumbent}: mean {t['words_lost_vs_incumbent'].get('mean','-')}, "
          f"max {t['words_lost_vs_incumbent'].get('max','-')}")
    print()
    print(f"wrote {out_dir/'summary.aggregate.json'}")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="replay the corpus through both services")
    r.add_argument("--a", required=True, help="incumbent:  name=url[,model=...]")
    r.add_argument("--b", required=True, help="challenger: name=url[,model=...]")
    r.add_argument("--corpus", default=str(DEFAULT_CORPUS))
    r.add_argument("--out", required=True)
    r.add_argument("--sessions", type=int, default=25, help="0 = every eligible session")
    r.add_argument("--seed", type=int, default=7)
    r.add_argument("--min-ms", type=int, default=600)
    r.add_argument("--max-ms", type=int, default=60000)
    r.add_argument("--endpoint-silence-ms", type=float, default=1500.0,
                   help="must match the setting the corpus was recorded under, or the "
                        "segmentation is not the one the live run used")
    r.add_argument("--nonspeech-per-session", type=int, default=2,
                   help="open-microphone gaps to probe for hallucination; -1 for all")
    r.add_argument("--nonspeech-min-ms", type=int, default=1500)
    r.add_argument("--nonspeech-max-ms", type=int, default=15000)
    r.add_argument("--service-filter", default=None,
                   help="only sessions whose start header names this service")
    r.add_argument("--min-reproduction", type=float, default=0.80)
    r.add_argument("--adjudicate", type=int, default=40)
    r.set_defaults(func=run)

    p = sub.add_parser("report", help="re-derive the summary from an existing run")
    p.add_argument("--out", required=True)
    p.add_argument("--min-reproduction", type=float, default=0.80)
    p.add_argument("--adjudicate", type=int, default=40)
    p.set_defaults(func=report)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
