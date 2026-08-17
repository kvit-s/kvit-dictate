#!/usr/bin/env python3
"""bench/record.py — corpus recorder for the Phase 1 bake-off.

Implements plan.md §3 Phase 1 step 2 (the corpus and its dev/held-out split) and §4's manual
`Win+H` comparison. Stdlib only, CPython 3.12, Windows.

────────────────────────────────────────────────────────────────────────────────────────────
THIS SCRIPT DOES NOT OPEN A MICROPHONE
────────────────────────────────────────────────────────────────────────────────────────────

plan.md §9 records the defect that produced this rule: revision 1 claimed the corpus would use
"the same WASAPI path as production" while scheduling WASAPI capture for Phase 3 and naming
`bench/record.py` as the recorder. Those cannot be the same path. The resolution was to build
the real `src/audio_capture.{h,cpp}` first and record the corpus *through it*.

So this file contains no audio library, no `sounddevice`, no `pyaudio`, no `winmm` capture. It
is a **driver and a bookkeeper**: it owns the prompting, the file naming, the metadata, the
ledger and the split discipline, and it shells out to a C++ recorder that is a thin `main()`
over `kd::audio::AudioCapture` + `kd::audio::WavWriter`. If the C++ tool is missing, this
script refuses to record rather than substituting a different capture chain — a corpus recorded
through a different microphone pipeline silently measures a different product.

`--simulate` exists for exercising the *bookkeeping* without a microphone. It writes into
`bench/corpus/_simulated/`, never into `dev/` or `heldout/`, and stamps every artifact
`"driver": "simulated"`. It is not a recording mode.

────────────────────────────────────────────────────────────────────────────────────────────
THE C++ RECORDER CONTRACT  (kd_record, contract version 1)
────────────────────────────────────────────────────────────────────────────────────────────

As of writing, `tools/probe/main.cpp` is a Phase 0B placeholder and no recording entry point
exists. What follows is the interface this script is written against; it is deliberately small
and stdout/stdin-only so the C++ side stays a ~150-line `main()`. Anything richer (a GUI, a
socket) would put more between the corpus and the production capture path, which is the exact
thing this design is avoiding.

  kd_record --list-devices
      stdout: exactly one JSON object, then exit 0:
        {"contract": 1,
         "tool_version": "...",           # human-readable, e.g. a git describe
         "kd_commit": "<sha or empty>",
         "devices": [{"id": "<endpoint id>",        # stable across reboots (DeviceInfo::id)
                      "friendly_name": "...",
                      "is_default": true,
                      "mix_format": {"sample_rate": 48000, "channels": 2, "format": "F32"}}]}

  kd_record --record --out-16k <path.wav> --meta <path.json>
            [--out-native <path.wav>] [--device <endpoint id>]
            [--max-seconds <float>] [--chunk-ms <int>]
      * Opens the device, starts capture, then prints a single line `READY` to stdout and
        flushes. Nothing before READY may be interpreted as audio having started.
      * Records until EITHER a line arrives on stdin (any content; `STOP` by convention),
        stdin reaches EOF, or --max-seconds elapses. Console CTRL_C is deliberately NOT the
        stop mechanism: it races the WAV size-patching in `WavWriter::close()`.
      * MAY print `LEVEL <dBFS>` lines while recording; this script displays them and
        tolerates their absence. Any other unrecognised line is ignored.
      * On stop: flushes the converter (`AudioConverter::flush`), closes both WAVs, writes
        --meta, prints `DONE`, exits 0.
      * Exit non-zero means nothing usable was produced; stderr carries the diagnostic.
      * --out-native writes the untouched device-format capture beside the 16 kHz stream —
        plan.md §3 Phase 1 step 1, so "the backend got this wrong" stays separable from "the
        resampler got this wrong". This script always asks for it.

  --meta JSON, written by kd_record:
      {"contract": 1, "tool_version": "...", "kd_commit": "...",
       "device": {"id","friendly_name","is_default"},
       "native_format": {"sample_rate","channels","format"},   # as captured
       "output_format": {"sample_rate":16000,"channels":1,"format":"s16le"},
       "frames_native": 0, "frames_16k": 0, "duration_ms": 0,
       "group_delay_ms": 2.85,                                  # AudioConverter::group_delay_ms
       "started_utc": "...", "ended_utc": "...",
       "discontinuities": [{"session_pos_ms":0,"gap_ms":0,"reason":"..."}],
       "stats": {"frames_captured":0,"discontinuities":0,"glitch_frames_estimated":0,
                 "sink_callbacks":0,"max_sink_callback_us":0}}   # AudioCapture::Stats verbatim

`contract` is checked on every invocation. A mismatch is a hard refusal, not a warning: the
corpus's meaning is downstream of the capture chain, and a silently changed recorder is a
silently changed corpus.

────────────────────────────────────────────────────────────────────────────────────────────
THE DEV / HELD-OUT SPLIT, ENFORCED STRUCTURALLY
────────────────────────────────────────────────────────────────────────────────────────────

plan.md §3 Phase 1: `heldout/` is the decision set — touched once, at selection time, never
tuned on. §6 lists "subjective model selection after seeing results" as a risk whose mitigation
is this split. A split that depends on operator discipline is not a mitigation, so:

1. **The split is pre-registered data, not a runtime choice.** Every prompt in
   `bench/prompts/*.jsonl` carries a `split` field, committed to git. The destination directory
   is derived from that field alone; `--split` only *filters* which prompts the session walks.
   Asking to record a dev prompt into heldout is not expressible.
2. **Recording held-out material takes three deliberate acts**: `--split heldout`, the
   `--yes-heldout` flag, and typing a confirmation phrase at an interactive prompt.
3. **Accepted held-out files are made read-only** on disk immediately after acceptance.
4. **There is no `promote`, `move`, or `rename` command, and there never should be.** Moving a
   clip between splits requires editing the committed prompt file — a reviewable diff — and
   re-recording. The append-only ledger still records the original location, so the move is
   visible afterwards.
5. **`verify` recomputes everything**: the ledger's hash chain, each file's SHA-256, and the
   agreement between each clip's directory, its sidecar's split, and the committed prompt
   file's split. Relocating a file by hand fails all three.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as _dt
import hashlib
import json
import math
import os
import shutil
import stat
import struct
import subprocess
import sys
import threading
import time
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, NoReturn, Sequence

# ── Versions and layout ─────────────────────────────────────────────────────────────────────

# Bumped when this script changes what it writes. Recorded in every sidecar so a mixed-vintage
# corpus is diagnosable rather than mysterious.
RECORD_PY_VERSION = 1

# The kd_record contract above. Bump both sides together, never one.
RECORDER_CONTRACT = 1

# Sidecar / ledger schema. Separate from RECORD_PY_VERSION because a cosmetic change to the
# script should not read as a change to the data format.
SIDECAR_SCHEMA = 1
LEDGER_SCHEMA = 1

BENCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = BENCH_DIR.parent
PROMPTS_DIR = BENCH_DIR / "prompts"
CORPUS_DIR = BENCH_DIR / "corpus"
LEDGER_PATH = CORPUS_DIR / "LEDGER.jsonl"
SIMULATED_DIR = CORPUS_DIR / "_simulated"

SPLITS = ("dev", "heldout")
# `clean` is not recorded here — it is a LibriSpeech slice, imported (see `import-clean`).
RECORDED_CATEGORIES = ("tech", "hard", "silence", "noise")
ALL_CATEGORIES = RECORDED_CATEGORIES + ("clean",)

PROMPT_FILES = {
    "tech": PROMPTS_DIR / "tech.jsonl",
    "hard": PROMPTS_DIR / "hard.jsonl",
    "silence": PROMPTS_DIR / "ambient.jsonl",
    "noise": PROMPTS_DIR / "ambient.jsonl",
}

HELDOUT_CONFIRMATION = "RECORD HELDOUT"

# Safety cap on a single take. A stuck operator or a stuck recorder should not fill the disk;
# `WavWriter` itself only fails at 4 GiB, which is ~5.6 hours.
DEFAULT_MAX_SECONDS = 90.0


# ── Small utilities ─────────────────────────────────────────────────────────────────────────


def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="milliseconds")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def canonical_json(obj: Any) -> str:
    """Stable serialisation, used for hashing. sort_keys + no spaces so the hash of a record
    does not depend on the order a dict happened to be built in."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def git_commit() -> str:
    """Best effort. An empty string is an honest answer; a fabricated one is not."""
    try:
        out = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10, check=False)
        return out.stdout.strip() if out.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def wav_duration_ms(path: Path) -> float | None:
    """Duration read from the file itself, so the recorder's own claim can be cross-checked."""
    try:
        with contextlib.closing(wave.open(str(path), "rb")) as w:
            rate = w.getframerate()
            return (w.getnframes() / rate) * 1000.0 if rate else None
    except (OSError, wave.Error):
        return None


def set_read_only(path: Path) -> None:
    """Held-out clips get the read-only attribute the moment they are accepted. This does not
    stop a determined operator — nothing in a single-user tool can — but it does stop the
    accidental overwrite, which is the realistic failure."""
    try:
        path.chmod(stat.S_IREAD)
    except OSError as exc:  # pragma: no cover - filesystem dependent
        warn(f"could not set read-only on {path}: {exc}")


def is_read_only(path: Path) -> bool:
    return not (path.stat().st_mode & stat.S_IWRITE)


# ── Console ─────────────────────────────────────────────────────────────────────────────────


def _reconfigure_stdio() -> None:
    """The prompt set contains `µ`-free but backslash- and symbol-heavy text; a cp1252 console
    would mangle it. PYTHONUTF8=1 is the real fix (it is itself one of the prompts), but do not
    depend on the operator having set it."""
    for stream in (sys.stdout, sys.stderr):
        with contextlib.suppress(Exception):
            stream.reconfigure(encoding="utf-8", errors="replace")


def say(msg: str = "") -> None:
    print(msg, flush=True)


def warn(msg: str) -> None:
    print(f"  ! {msg}", file=sys.stderr, flush=True)


def fail(msg: str, code: int = 2) -> NoReturn:
    print(f"error: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(code)


RULE = "─" * 88


# ── Prompts ─────────────────────────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class Prompt:
    id: str
    category: str
    split: str
    text: str
    terms: tuple[str, ...]
    delivery: str | None
    notes: str
    duration_s: float | None
    source_file: Path
    source_sha256: str

    @property
    def is_ambient(self) -> bool:
        """silence/noise clips have an empty reference: any output at all is a hallucination
        (plan.md §3 Phase 1 step 2, §4)."""
        return self.category in ("silence", "noise")


def load_prompts(categories: Sequence[str]) -> list[Prompt]:
    wanted = set(categories)
    seen_ids: dict[str, Path] = {}
    prompts: list[Prompt] = []

    for path in sorted({PROMPT_FILES[c] for c in wanted if c in PROMPT_FILES}):
        if not path.exists():
            fail(f"prompt file missing: {path}")
        digest = sha256_file(path)
        with path.open("r", encoding="utf-8") as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError as exc:
                    fail(f"{path}:{lineno}: {exc}")
                for key in ("id", "category", "split", "text"):
                    if key not in row:
                        fail(f"{path}:{lineno}: missing required field {key!r}")
                if row["category"] not in ALL_CATEGORIES:
                    fail(f"{path}:{lineno}: unknown category {row['category']!r}")
                if row["split"] not in SPLITS:
                    fail(f"{path}:{lineno}: split must be one of {SPLITS}, got {row['split']!r}")
                if row["id"] in seen_ids:
                    # A duplicate id would make the split ambiguous, which is the one thing the
                    # split may never be.
                    fail(f"{path}:{lineno}: duplicate prompt id {row['id']!r} "
                         f"(also in {seen_ids[row['id']]})")
                seen_ids[row["id"]] = path
                if row["category"] not in wanted:
                    continue
                if not row["text"] and row["category"] not in ("silence", "noise"):
                    fail(f"{path}:{lineno}: empty text is only legal for silence/noise")
                prompts.append(Prompt(
                    id=row["id"], category=row["category"], split=row["split"],
                    text=row["text"], terms=tuple(row.get("terms") or ()),
                    delivery=row.get("delivery"), notes=row.get("notes", ""),
                    duration_s=row.get("duration_s"),
                    source_file=path, source_sha256=digest))
    return prompts


def prompt_registry() -> dict[str, Prompt]:
    """Every prompt in every file, keyed by id — the authority `verify` checks splits against."""
    return {p.id: p for p in load_prompts(RECORDED_CATEGORIES)}


# ── Corpus layout ───────────────────────────────────────────────────────────────────────────
#
# corpus/<split>/<category>/<id>.16k.wav        <- THE accepted take; what the harness reads
#                          /<id>.16k.json       <- its sidecar (reference text + provenance)
#                          /<id>.native.wav     <- the same take, untouched device format
#                          /<id>.recorder.json  <- kd_record's own metadata for that take
#                          /takes/              <- every superseded and discarded take
#                                <id>.take-NN.16k.wav / .native.wav / .json / .discarded
# corpus/<split>/winh/<id>.json                 <- Win+H comparison artifacts (plan.md §4)
# corpus/LEDGER.jsonl                           <- append-only, hash-chained
#
# **The flat top level is a hard interop requirement, not a preference.** bench/run_matrix.py
# discovers clips with a NON-recursive `corpus/<split>/<category>/*.16k.wav` and reads the
# reference from `<stem>.json` — i.e. `<id>.16k.json`. Two consequences that shaped this layout:
#
#   * a rejected or superseded take must not sit beside the accepted one, or the harness would
#     silently score it as a second clip. Hence `takes/`, which the non-recursive glob ignores.
#   * the accepted take is MOVED, never copied. One recording, one file. Two copies of the same
#     audio is two things that can disagree.
#
# The whole tree is gitignored (.gitignore: `bench/corpus/`), per plan.md §1.9.


def category_dir(prompt: Prompt, simulated: bool = False) -> Path:
    root = SIMULATED_DIR if simulated else CORPUS_DIR
    # NOTE: the split component comes from the prompt, never from a command-line argument.
    return root / prompt.split / prompt.category


@dataclass
class ClipPaths:
    """Where one prompt's audio lives, in both of its states."""

    root: Path          # corpus/<split>/<category>
    clip_id: str
    take: int

    # -- the accepted take: flat, stable names, what the harness reads -------------------
    @property
    def accepted_wav16k(self) -> Path:
        return self.root / f"{self.clip_id}.16k.wav"

    @property
    def accepted_native(self) -> Path:
        return self.root / f"{self.clip_id}.native.wav"

    @property
    def accepted_sidecar(self) -> Path:
        # `<id>.16k.wav` -> stem `<id>.16k` -> sidecar `<id>.16k.json`, which is precisely what
        # run_matrix.py's _read_reference() looks for. Do not rename without changing both.
        return self.root / f"{self.clip_id}.16k.json"

    @property
    def accepted_recorder_meta(self) -> Path:
        return self.root / f"{self.clip_id}.recorder.json"

    def accepted_set(self) -> list[Path]:
        return [p for p in (self.accepted_wav16k, self.accepted_native,
                            self.accepted_sidecar, self.accepted_recorder_meta) if p.exists()]

    # -- the in-progress / superseded take ------------------------------------------------
    @property
    def takes_dir(self) -> Path:
        return self.root / "takes"

    def _take(self, suffix: str, take: int | None = None) -> Path:
        return self.takes_dir / f"{self.clip_id}.take-{(take or self.take):02d}.{suffix}"

    @property
    def take_wav16k(self) -> Path:
        return self._take("16k.wav")

    @property
    def take_native(self) -> Path:
        return self._take("native.wav")

    @property
    def take_sidecar(self) -> Path:
        return self._take("json")

    @property
    def take_recorder_meta(self) -> Path:
        return self._take("recorder.json")

    @property
    def discarded_marker(self) -> Path:
        return self._take("discarded")

    def take_set(self) -> list[Path]:
        return [p for p in (self.take_wav16k, self.take_native,
                            self.take_sidecar, self.take_recorder_meta) if p.exists()]

    def next_take_number(self) -> int:
        """Take numbers are never reused — not by a discarded take (a `.discarded` marker keeps
        the number consumed) and not by a superseded one. Two different recordings sharing a
        name in the ledger would make the ledger useless as evidence."""
        highest = 0
        if self.takes_dir.is_dir():
            for entry in self.takes_dir.glob(f"{self.clip_id}.take-*"):
                part = entry.name[len(self.clip_id) + len(".take-"):]
                with contextlib.suppress(ValueError):
                    highest = max(highest, int(part.split(".")[0]))
        if self.accepted_sidecar.is_file():
            with contextlib.suppress(OSError, json.JSONDecodeError, TypeError):
                data = json.loads(self.accepted_sidecar.read_text(encoding="utf-8"))
                highest = max(highest, int(data.get("take", 0)))
        return highest + 1


# ── Ledger ──────────────────────────────────────────────────────────────────────────────────


class Ledger:
    """Append-only, hash-chained event log.

    Each entry stores the hash of its predecessor, so deleting or editing an entry after the
    fact invalidates every entry after it. This is not tamper-*proofing* — anyone able to edit
    the ledger can rewrite the whole chain — it is tamper-*evidence*, and that is the property
    that matters here: an accidental edit, a half-finished manual reorganisation, or a clip
    quietly relabelled between splits shows up in `verify` instead of vanishing.
    """

    GENESIS = "genesis"

    def __init__(self, path: Path = LEDGER_PATH) -> None:
        self.path = path

    def _entries(self) -> list[dict]:
        if not self.path.exists():
            return []
        out = []
        with self.path.open("r", encoding="utf-8") as f:
            for lineno, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError as exc:
                    fail(f"{self.path}:{lineno}: ledger is not valid JSONL ({exc})")
        return out

    def entries(self) -> list[dict]:
        return self._entries()

    @staticmethod
    def _hash(entry: dict, prev: str) -> str:
        body = {k: v for k, v in entry.items() if k != "hash"}
        return hashlib.sha256((prev + canonical_json(body)).encode("utf-8")).hexdigest()

    def append(self, event: str, **fields: Any) -> dict:
        entries = self._entries()
        prev = entries[-1]["hash"] if entries else self.GENESIS
        entry = {
            "schema": LEDGER_SCHEMA,
            "n": len(entries) + 1,
            "utc": utc_now(),
            "event": event,
            "prev": prev,
            **fields,
        }
        entry["hash"] = self._hash(entry, prev)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("a", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
        return entry

    def verify_chain(self) -> list[str]:
        problems: list[str] = []
        prev = self.GENESIS
        for i, entry in enumerate(self._entries(), 1):
            if entry.get("n") != i:
                problems.append(f"ledger entry {i}: n={entry.get('n')!r}, expected {i}")
            if entry.get("prev") != prev:
                problems.append(f"ledger entry {i}: prev hash does not chain")
            expected = self._hash(entry, entry.get("prev", ""))
            if entry.get("hash") != expected:
                problems.append(f"ledger entry {i}: hash mismatch (entry was edited)")
            prev = entry.get("hash", "")
        return problems

    def accepted_takes(self) -> dict[str, dict]:
        """prompt id -> the ledger entry of its most recently accepted take."""
        out: dict[str, dict] = {}
        for entry in self._entries():
            if entry.get("event") == "take_accepted":
                out[entry["prompt_id"]] = entry
        return out


# ── The recorder drivers ────────────────────────────────────────────────────────────────────


@dataclass
class RecordRequest:
    wav16k: Path
    wav_native: Path | None
    meta: Path
    device_id: str | None
    max_seconds: float
    chunk_ms: int = 20


class RecorderUnavailable(RuntimeError):
    pass


class RecorderFailed(RuntimeError):
    pass


def find_recorder(explicit: str | None) -> Path:
    """Locate kd_record.exe.

    Order: --recorder, $KD_RECORDER, then the usual VS-generator output directories. The VS
    generator (plan.md §5.3 — Ninja is not on PATH) puts binaries in a per-configuration
    subdirectory, hence the Release/Debug/RelWithDebInfo sweep.
    """
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    env = os.environ.get("KD_RECORDER")
    if env:
        candidates.append(Path(env))
    for build in (REPO_ROOT / "build", REPO_ROOT / "build-release", REPO_ROOT / "out"):
        for sub in ("", "tools", "tools/record"):
            for cfg in ("RelWithDebInfo", "Release", "Debug", ""):
                candidates.append(build / sub / cfg / "kd_record.exe")
    for c in candidates:
        if c.is_file():
            return c
    # Last resort: anywhere under a build tree. Reported, not silent — picking a binary by
    # search is exactly how you end up benchmarking a stale build.
    for build in (REPO_ROOT / "build", REPO_ROOT / "build-release", REPO_ROOT / "out"):
        if build.is_dir():
            for found in sorted(build.rglob("kd_record.exe")):
                warn(f"using {found} (found by search, not by convention) — check it is current")
                return found
    raise RecorderUnavailable(
        "kd_record.exe not found.\n"
        "  Looked at: " + ", ".join(str(c) for c in candidates[:4]) + ", ...\n"
        "  This script drives the REAL C++ capture path on purpose (plan.md §3 Phase 1) and\n"
        "  will not fall back to a Python audio library. Build the recorder, or pass\n"
        "  --recorder <path>, or use --simulate to exercise the bookkeeping only.")


class _LineReader(threading.Thread):
    """Drains a child's stdout on a thread so a full pipe buffer can never wedge the child
    while the main thread is blocked on the operator."""

    def __init__(self, stream, on_line: Callable[[str], None]) -> None:
        super().__init__(daemon=True)
        self._stream = stream
        self._on_line = on_line
        self.lines: list[str] = []

    def run(self) -> None:  # pragma: no cover - thread body
        try:
            for raw in self._stream:
                line = raw.rstrip("\r\n")
                self.lines.append(line)
                self._on_line(line)
        except (OSError, ValueError):
            pass


class KdRecordDriver:
    """Drives `kd_record.exe`. See the contract in the module docstring."""

    name = "kd_record"

    def __init__(self, exe: Path) -> None:
        self.exe = exe
        self._identity: dict | None = None

    # -- device enumeration ---------------------------------------------------------------

    def identity(self) -> dict:
        if self._identity is None:
            self._identity = self._list_devices_raw()
        return self._identity

    def _list_devices_raw(self) -> dict:
        proc = subprocess.run([str(self.exe), "--list-devices"],
                              capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=60, check=False)
        if proc.returncode != 0:
            raise RecorderFailed(
                f"{self.exe.name} --list-devices exited {proc.returncode}\n{proc.stderr.strip()}")
        try:
            data = json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            raise RecorderFailed(
                f"{self.exe.name} --list-devices did not print JSON: {exc}\n"
                f"stdout was: {proc.stdout[:400]!r}") from exc
        self._check_contract(data)
        return data

    @staticmethod
    def _check_contract(data: dict) -> None:
        got = data.get("contract")
        if got != RECORDER_CONTRACT:
            # Hard refusal. The corpus's meaning depends on the capture chain; a recorder that
            # has changed its contract may have changed its conversion too.
            raise RecorderFailed(
                f"recorder contract mismatch: kd_record reports {got!r}, this script speaks "
                f"{RECORDER_CONTRACT}. Update both together (see bench/record.py docstring).")

    def devices(self) -> list[dict]:
        return list(self.identity().get("devices", []))

    # -- recording ------------------------------------------------------------------------

    def record(self, req: RecordRequest, wait_for_stop: Callable[[], None]) -> dict:
        argv = [str(self.exe), "--record",
                "--out-16k", str(req.wav16k),
                "--meta", str(req.meta),
                "--max-seconds", f"{req.max_seconds:.3f}",
                "--chunk-ms", str(req.chunk_ms)]
        if req.wav_native is not None:
            argv += ["--out-native", str(req.wav_native)]
        if req.device_id:
            argv += ["--device", req.device_id]

        ready = threading.Event()
        done = threading.Event()

        def on_line(line: str) -> None:
            if line.strip() == "READY":
                ready.set()
            elif line.strip() == "DONE":
                done.set()
            elif line.startswith("LEVEL "):
                # Optional per the contract; purely a comfort for the operator.
                sys.stdout.write(f"\r    level {line[6:]:>10}   ")
                sys.stdout.flush()

        proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1)
        reader = _LineReader(proc.stdout, on_line)
        reader.start()
        err_reader = _LineReader(proc.stderr, lambda _l: None)
        err_reader.start()

        # Wait for READY, but not forever, and notice a child that died instead.
        deadline = time.monotonic() + 20.0
        while not ready.is_set() and time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RecorderFailed(
                    f"{self.exe.name} exited {proc.returncode} before printing READY\n"
                    + "\n".join(err_reader.lines[-10:]))
            time.sleep(0.02)
        if not ready.is_set():
            proc.kill()
            raise RecorderFailed(f"{self.exe.name} never printed READY (device stuck?)")

        try:
            wait_for_stop()
        finally:
            # Even on an exception we must stop the child; an orphaned recorder holds the mic
            # open, which plan.md §1.9 makes a visible-state promise about.
            with contextlib.suppress(OSError, ValueError):
                if proc.stdin and not proc.stdin.closed:
                    proc.stdin.write("STOP\n")
                    proc.stdin.flush()
                    proc.stdin.close()

        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise RecorderFailed(f"{self.exe.name} did not exit within 30 s of STOP") from None

        if proc.returncode != 0:
            raise RecorderFailed(
                f"{self.exe.name} exited {proc.returncode}\n"
                + "\n".join(err_reader.lines[-10:]))
        if not req.meta.exists():
            raise RecorderFailed(f"{self.exe.name} exited 0 but wrote no metadata at {req.meta}")

        meta = json.loads(req.meta.read_text(encoding="utf-8"))
        self._check_contract(meta)
        return meta


class SimulatedDriver:
    """Bookkeeping-only stand-in. NOT a recording mode.

    Writes a short synthetic tone so the file-handling, ledger and verify paths can be
    exercised on a machine with no microphone and no C++ build. Everything it produces is
    stamped `"driver": "simulated"` and lands under corpus/_simulated/, which no metric run
    reads. It exists because the alternative — testing the bookkeeping by recording real
    audio — makes the tests depend on a human.
    """

    name = "simulated"

    def identity(self) -> dict:
        return {"contract": RECORDER_CONTRACT, "tool_version": "simulated",
                "kd_commit": "", "devices": self.devices()}

    def devices(self) -> list[dict]:
        return [{"id": "SIMULATED", "friendly_name": "simulated device", "is_default": True,
                 "mix_format": {"sample_rate": 48000, "channels": 2, "format": "F32"}}]

    def record(self, req: RecordRequest, wait_for_stop: Callable[[], None]) -> dict:
        started = utc_now()
        t0 = time.monotonic()
        wait_for_stop()
        seconds = min(max(time.monotonic() - t0, 0.25), req.max_seconds)

        self._tone(req.wav16k, 16000, 1, seconds)
        if req.wav_native is not None:
            self._tone(req.wav_native, 48000, 1, seconds)

        meta = {
            "contract": RECORDER_CONTRACT, "tool_version": "simulated", "kd_commit": "",
            "device": {"id": "SIMULATED", "friendly_name": "simulated device",
                       "is_default": True},
            "native_format": {"sample_rate": 48000, "channels": 1, "format": "S16"},
            "output_format": {"sample_rate": 16000, "channels": 1, "format": "s16le"},
            "frames_native": int(48000 * seconds), "frames_16k": int(16000 * seconds),
            "duration_ms": seconds * 1000.0, "group_delay_ms": 0.0,
            "started_utc": started, "ended_utc": utc_now(),
            "discontinuities": [],
            "stats": {"frames_captured": int(48000 * seconds), "discontinuities": 0,
                      "glitch_frames_estimated": 0, "sink_callbacks": 0,
                      "max_sink_callback_us": 0},
        }
        req.meta.write_text(json.dumps(meta, indent=2), encoding="utf-8")
        return meta

    @staticmethod
    def _tone(path: Path, rate: int, channels: int, seconds: float) -> None:
        n = int(rate * seconds)
        frames = bytearray()
        for i in range(n):
            v = int(6000 * math.sin(2 * math.pi * 220.0 * i / rate))
            frames += struct.pack("<h", v) * channels
        path.parent.mkdir(parents=True, exist_ok=True)
        with contextlib.closing(wave.open(str(path), "wb")) as w:
            w.setnchannels(channels)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(bytes(frames))


# ── Interactive helpers ─────────────────────────────────────────────────────────────────────


def ask(prompt: str, allowed: Iterable[str], default: str | None = None) -> str:
    allowed = list(allowed)
    while True:
        try:
            raw = input(prompt).strip().lower()
        except EOFError:
            raise SystemExit("\naborted (stdin closed)") from None
        except KeyboardInterrupt:
            raise SystemExit("\naborted") from None
        if not raw and default is not None:
            return default
        if raw in allowed:
            return raw
        say(f"    choose one of: {', '.join(allowed)}")


def read_line(prompt: str) -> str:
    try:
        return input(prompt)
    except (EOFError, KeyboardInterrupt):
        raise SystemExit("\naborted") from None


def play(path: Path) -> None:
    """Playback via `winsound`, which is in the stdlib on Windows. Failure to play back is a
    convenience problem, never a recording problem, so it is caught and reported."""
    try:
        import winsound  # noqa: PLC0415 - Windows-only stdlib module, imported where used
        winsound.PlaySound(str(path), winsound.SND_FILENAME)
    except Exception as exc:  # pragma: no cover - depends on the audio stack
        warn(f"playback failed ({exc}); the file itself is fine")


# ── The recording session ───────────────────────────────────────────────────────────────────


@dataclass
class SessionOptions:
    split: str
    categories: tuple[str, ...]
    only: tuple[str, ...] = ()
    device_id: str | None = None
    operator: str = ""
    simulate: bool = False
    redo: bool = False           # re-record prompts that already have an accepted take
    max_seconds: float = DEFAULT_MAX_SECONDS
    chunk_ms: int = 20
    notes: str = ""
    session_tag: str = field(default_factory=lambda: utc_now())


def confirm_heldout(opts: SessionOptions, yes_flag: bool) -> None:
    """Three deliberate acts, per the split discipline in the module docstring."""
    if opts.split != "heldout":
        return
    if not yes_flag:
        fail("recording into heldout/ requires --yes-heldout.\n"
             "  heldout/ is the DECISION SET (plan.md §3 Phase 1): touched once, at selection\n"
             "  time, never tuned on. If you are iterating on VAD, prompts, normalisation or\n"
             "  stability parameters, you want --split dev.")
    if not sys.stdin.isatty():
        fail("refusing to record heldout/ non-interactively — the confirmation prompt is the "
             "third of three deliberate acts and cannot be scripted away.")
    say()
    say(RULE)
    say("  You are about to record into the HELD-OUT set.")
    say("  This is the set the pre-registered selection rule (plan.md §4) is evaluated on.")
    say("  It is recorded once and never tuned against. Files become read-only on accept.")
    say(RULE)
    typed = read_line(f'  Type exactly "{HELDOUT_CONFIRMATION}" to continue: ').strip()
    if typed != HELDOUT_CONFIRMATION:
        fail("confirmation phrase not matched — nothing recorded.")
    say()


def make_driver(recorder: str | None, simulate: bool):
    if simulate:
        say("  *** SIMULATED DRIVER — no microphone is opened, no corpus audio is produced. ***")
        say(f"  *** Output goes to {SIMULATED_DIR} and is stamped simulated.          ***")
        return SimulatedDriver()
    return KdRecordDriver(find_recorder(recorder))


def build_sidecar(prompt: Prompt, paths: ClipPaths, recorder_meta: dict, opts: SessionOptions,
                  driver_name: str, operator_note: str, measured_ms: float | None) -> dict:
    """The sidecar that sits beside the accepted clip.

    `prompt_text` is the key bench/run_matrix.py reads as the reference, so it is the reference:
    verbatim as the prompt was written and as the operator was asked to speak it, with no
    normalisation. Normalisation is metrics.py's business, and plan.md §4 requires raw and
    normalised to be reported side by side precisely so nobody can normalise the interesting
    errors away here, upstream, where it would be invisible.
    """
    return {
        "schema": SIDECAR_SCHEMA,
        "record_py_version": RECORD_PY_VERSION,
        "recorder_contract": RECORDER_CONTRACT,
        "driver": driver_name,
        "simulated": driver_name == "simulated",
        "kd_commit": recorder_meta.get("kd_commit", "") or git_commit(),

        # Identity of the prompt AS COMMITTED. `prompt_source_sha256` is the hash of the whole
        # prompt file: if a prompt is later reworded, every clip recorded against the old
        # wording is identifiable instead of silently mismatched against a new reference.
        "prompt_id": prompt.id,
        "category": prompt.category,
        "split": prompt.split,
        "prompt_text": prompt.text,
        "prompt_terms": list(prompt.terms),
        "prompt_delivery": prompt.delivery,
        "prompt_source": str(prompt.source_file.relative_to(REPO_ROOT)),
        "prompt_source_sha256": prompt.source_sha256,

        "take": paths.take,
        "recorded_utc": recorder_meta.get("started_utc", utc_now()),
        "operator": opts.operator,
        "operator_note": operator_note,
        "session_notes": opts.notes,
        "session_tag": opts.session_tag,

        "device": recorder_meta.get("device", {}),
        "native_format": recorder_meta.get("native_format", {}),
        "output_format": recorder_meta.get("output_format", {}),
        "duration_ms_reported": recorder_meta.get("duration_ms"),
        "duration_ms_measured": measured_ms,
        "group_delay_ms": recorder_meta.get("group_delay_ms"),
        "discontinuities": recorder_meta.get("discontinuities", []),
        "capture_stats": recorder_meta.get("stats", {}),

        "files": {
            "wav_16k": paths.accepted_wav16k.name,
            "wav_native": (paths.accepted_native.name
                           if paths.accepted_native.exists() else None),
            "recorder_meta": (paths.accepted_recorder_meta.name
                              if paths.accepted_recorder_meta.exists() else None),
        },
        "sha256": {
            "wav_16k": sha256_file(paths.accepted_wav16k),
            "wav_native": (sha256_file(paths.accepted_native)
                           if paths.accepted_native.exists() else None),
        },
    }


def show_prompt(prompt: Prompt, index: int, total: int) -> None:
    say()
    say(RULE)
    tag = "HELD OUT" if prompt.split == "heldout" else "dev"
    say(f"  [{index}/{total}]  {prompt.id}   {prompt.category}   {tag}")
    say(RULE)
    if prompt.is_ambient:
        say(f"  AMBIENT CLIP — {prompt.duration_s:.0f} s, no speech.")
    else:
        say()
        for line in _wrap(prompt.text, 84):
            say(f"    {line}")
        say()
    if prompt.delivery:
        say(f"  DELIVERY: {prompt.delivery}")
    if prompt.terms:
        say(f"  terms:    {', '.join(prompt.terms)}")


def _wrap(text: str, width: int) -> list[str]:
    words, lines, cur = text.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width:
            lines.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}".strip()
    if cur:
        lines.append(cur)
    return lines or [""]


def supersede_accepted(paths: ClipPaths, prompt: Prompt, ledger: Ledger,
                       session_tag: str) -> None:
    """Move a previously accepted take into `takes/` before a new one replaces it.

    Nothing is ever deleted or overwritten here; the old recording keeps its take number and
    stays on disk. For a held-out clip the read-only bit must be cleared to move the file — the
    clearing is deliberate, ledgered, and only reachable through the full held-out
    confirmation, which is the point at which "this is a decision-set edit" has been stated
    three times.
    """
    existing = paths.accepted_set()
    if not existing:
        return
    old_take = 0
    if paths.accepted_sidecar.is_file():
        with contextlib.suppress(OSError, json.JSONDecodeError, TypeError):
            old_take = int(json.loads(
                paths.accepted_sidecar.read_text(encoding="utf-8")).get("take", 0))
    paths.takes_dir.mkdir(parents=True, exist_ok=True)
    moved = []
    for src in existing:
        suffix = src.name[len(paths.clip_id) + 1:]          # "16k.wav", "native.wav", …
        dst = paths.takes_dir / f"{paths.clip_id}.take-{old_take:02d}.{suffix}"
        with contextlib.suppress(OSError):
            src.chmod(stat.S_IWRITE | stat.S_IREAD)
        shutil.move(str(src), str(dst))
        moved.append(dst.name)
    ledger.append("take_superseded", prompt_id=prompt.id, split=prompt.split,
                  take=old_take, moved_to=moved, session_tag=session_tag)
    say(f"  previous take {old_take} moved to takes/ — nothing was deleted.")


def record_one(driver, prompt: Prompt, opts: SessionOptions, ledger: Ledger) -> bool:
    """Record, review and accept/reject one prompt. Returns False if the operator quit."""
    root = category_dir(prompt, simulated=opts.simulate)
    root.mkdir(parents=True, exist_ok=True)

    while True:
        say()
        choice = ask("  [Enter] record   [s]kip   [n]ote   [q]uit  > ",
                     ["", "s", "n", "q"], default="")
        if choice == "q":
            return False
        if choice == "s":
            ledger.append("prompt_skipped", prompt_id=prompt.id, split=prompt.split,
                          category=prompt.category, session_tag=opts.session_tag)
            return True
        if choice == "n":
            note = read_line("  note: ")
            ledger.append("operator_note", prompt_id=prompt.id, split=prompt.split,
                          note=note, session_tag=opts.session_tag)
            continue

        paths = ClipPaths(root, prompt.id, 0)          # take number resolved on the next line
        paths = ClipPaths(root, prompt.id, paths.next_take_number())
        paths.takes_dir.mkdir(parents=True, exist_ok=True)
        clash = paths.take_set()
        if clash:
            # Takes are never overwritten. If this fires, the take counter and the filesystem
            # disagree, which means something outside this script touched the corpus.
            fail(f"refusing to overwrite existing take files: {[str(p) for p in clash]}")

        max_seconds = (prompt.duration_s + 5.0) if prompt.is_ambient else opts.max_seconds
        req = RecordRequest(wav16k=paths.take_wav16k, wav_native=paths.take_native,
                            meta=paths.take_recorder_meta, device_id=opts.device_id,
                            max_seconds=max_seconds, chunk_ms=opts.chunk_ms)

        if prompt.is_ambient:
            def wait_for_stop(seconds=float(prompt.duration_s)) -> None:
                say(f"  ● RECORDING {seconds:.0f} s of {prompt.category} — stay quiet.")
                end = time.monotonic() + seconds
                while True:
                    left = end - time.monotonic()
                    if left <= 0:
                        break
                    sys.stdout.write(f"\r    {left:5.1f} s remaining   ")
                    sys.stdout.flush()
                    time.sleep(min(0.2, left))
                sys.stdout.write("\r" + " " * 40 + "\r")
        else:
            def wait_for_stop() -> None:
                say("  ● RECORDING — speak the sentence, then press [Enter] to stop.")
                read_line("")

        try:
            meta = driver.record(req, wait_for_stop)
        except RecorderFailed as exc:
            warn(str(exc))
            for p in paths.take_set():
                with contextlib.suppress(OSError):
                    p.unlink()
            paths.discarded_marker.write_text(f"recorder failed: {exc}\n", encoding="utf-8")
            ledger.append("take_failed", prompt_id=prompt.id, split=prompt.split,
                          take=paths.take, error=str(exc)[:500], session_tag=opts.session_tag)
            continue

        measured = wav_duration_ms(paths.take_wav16k)
        reported = meta.get("duration_ms")
        if measured is not None and isinstance(reported, (int, float)):
            if abs(measured - reported) > 50.0:
                # A disagreement here means the WAV header and the capture accounting differ —
                # a WavWriter or converter bug, and exactly the class of thing that would
                # otherwise be discovered as "the model is inexplicably bad on this clip".
                warn(f"duration mismatch: WAV says {measured:.0f} ms, recorder says "
                     f"{reported:.0f} ms — investigate before trusting this clip")

        discs = meta.get("stats", {}).get("discontinuities", 0)
        say(f"  take {paths.take}: {(measured or 0) / 1000.0:.1f} s, "
            f"{discs} discontinuit{'y' if discs == 1 else 'ies'}, "
            f"native {meta.get('native_format', {}).get('sample_rate', '?')} Hz")
        if discs:
            warn("this take has a capture glitch — retake it unless you are testing glitches")

        while True:
            what = ask("  [a]ccept   [r]etake   [d]iscard   [p]lay  > ", ["a", "r", "d", "p"])
            if what == "p":
                play(paths.take_wav16k)
                continue
            break

        if what in ("d", "r"):
            for p in paths.take_set():
                with contextlib.suppress(OSError):
                    p.unlink()
            paths.discarded_marker.write_text(
                f"discarded by operator at {utc_now()} ({'retake' if what == 'r' else 'discard'})\n",
                encoding="utf-8")
            ledger.append("take_discarded", prompt_id=prompt.id, split=prompt.split,
                          take=paths.take, session_tag=opts.session_tag)
            if what == "d":
                return True
            continue

        note = read_line("  note (Enter for none): ").strip()

        # Promote: the old accepted take (if any) steps aside, then this take MOVES into the
        # flat position the harness reads. Move, not copy — one recording, one file.
        supersede_accepted(paths, prompt, ledger, opts.session_tag)
        for src, dst in ((paths.take_wav16k, paths.accepted_wav16k),
                         (paths.take_native, paths.accepted_native),
                         (paths.take_recorder_meta, paths.accepted_recorder_meta)):
            if src.exists():
                shutil.move(str(src), str(dst))

        sidecar = build_sidecar(prompt, paths, meta, opts, driver.name, note, measured)
        paths.accepted_sidecar.write_text(json.dumps(sidecar, indent=2, ensure_ascii=False),
                                          encoding="utf-8")

        rel = str(paths.root.relative_to(REPO_ROOT)).replace("\\", "/")
        ledger.append(
            "take_recorded", prompt_id=prompt.id, split=prompt.split,
            category=prompt.category, take=paths.take, driver=driver.name,
            simulated=opts.simulate, session_tag=opts.session_tag, path=rel,
            files=sidecar["files"], sha256=sidecar["sha256"],
            sidecar_sha256=sha256_file(paths.accepted_sidecar))
        ledger.append(
            "take_accepted", prompt_id=prompt.id, split=prompt.split, take=paths.take,
            session_tag=opts.session_tag, path=rel)

        if prompt.split == "heldout" and not opts.simulate:
            for p in paths.accepted_set():
                set_read_only(p)
            say("  held-out take accepted and made read-only.")
        else:
            say("  accepted.")
        return True


def cmd_record(args: argparse.Namespace) -> int:
    categories = tuple(args.category) if args.category else RECORDED_CATEGORIES
    for c in categories:
        if c not in RECORDED_CATEGORIES:
            fail(f"{c!r} is not a recordable category. clean/ is imported, not recorded "
                 f"(see `import-clean`).")

    opts = SessionOptions(
        split=args.split, categories=categories, only=tuple(args.only or ()),
        device_id=args.device, operator=args.operator or os.environ.get("USERNAME", ""),
        simulate=args.simulate, redo=args.redo, max_seconds=args.max_seconds,
        chunk_ms=args.chunk_ms, notes=args.notes or "")

    confirm_heldout(opts, args.yes_heldout)

    prompts = [p for p in load_prompts(categories) if p.split == opts.split]
    if opts.only:
        wanted = set(opts.only)
        prompts = [p for p in prompts if p.id in wanted]
        missing = wanted - {p.id for p in prompts}
        if missing:
            fail(f"no such prompt id in split {opts.split!r}: {', '.join(sorted(missing))}")
    if not prompts:
        fail(f"no prompts matched (split={opts.split}, categories={','.join(categories)})")

    # A simulated session gets its own ledger. Otherwise `status` would count synthetic tones
    # as recorded corpus, which is the one lie this whole file exists to prevent.
    ledger = Ledger(SIMULATED_DIR / "LEDGER.jsonl" if opts.simulate else LEDGER_PATH)
    already = ledger.accepted_takes()
    if not opts.redo:
        skipped = [p for p in prompts if p.id in already]
        prompts = [p for p in prompts if p.id not in already]
        if skipped:
            say(f"  {len(skipped)} prompt(s) already have an accepted take; "
                f"pass --redo to record them again.")
    if not prompts:
        say("  nothing left to record.")
        return 0

    driver = make_driver(args.recorder, opts.simulate)
    ident = driver.identity()

    say()
    say(f"  recorder      {driver.name}  (tool_version {ident.get('tool_version', '?')})")
    say(f"  split         {opts.split}")
    say(f"  categories    {', '.join(categories)}")
    say(f"  prompts       {len(prompts)}")
    say(f"  operator      {opts.operator or '(unset)'}")
    if opts.device_id:
        say(f"  device        {opts.device_id}")
    else:
        default = next((d for d in driver.devices() if d.get("is_default")), None)
        say(f"  device        default → {default.get('friendly_name') if default else '?'}")

    ledger.append("session_started", split=opts.split, categories=list(categories),
                  driver=driver.name, simulated=opts.simulate, operator=opts.operator,
                  session_tag=opts.session_tag, record_py_version=RECORD_PY_VERSION,
                  prompt_sources={p.category: p.source_sha256 for p in prompts})

    done = 0
    for i, prompt in enumerate(prompts, 1):
        show_prompt(prompt, i, len(prompts))
        if not record_one(driver, prompt, opts, ledger):
            break
        done += 1

    ledger.append("session_ended", split=opts.split, session_tag=opts.session_tag,
                  prompts_completed=done, prompts_planned=len(prompts))
    say()
    say(f"  session done: {done}/{len(prompts)} prompts.")
    return 0


# ── Win+H comparison (plan.md §4) ───────────────────────────────────────────────────────────


def cmd_winh(args: argparse.Namespace) -> int:
    """The manual Win+H comparison plan.md §4 requires.

    "Not a scientific benchmark — a sanity check that the system actually improves the failure
    mode it exists to replace. Shipping something worse than the built-in would be the real
    failure."

    The operator speaks the same sentences into Win+H in a real target application and pastes
    what it produced. What is captured, and why:

      * the target app and Windows build, because Win+H's behaviour is not constant across
        either and an undated comparison ages into folklore;
      * whether the operator repeated the sentence, and how many times — a comparison where one
        side got three attempts is not a comparison;
      * the raw text exactly as pasted, unnormalised. metrics.py owns normalisation, and it
        reports raw and normalised side by side precisely so a normaliser cannot quietly
        rescue "llama dot cpp".
    """
    categories = tuple(args.category) if args.category else ("tech", "hard")
    prompts = [p for p in load_prompts(categories)
               if p.split == args.split and not p.is_ambient]
    if args.only:
        wanted = set(args.only)
        prompts = [p for p in prompts if p.id in wanted]
    if not prompts:
        fail("no prompts matched")

    if args.split == "heldout" and not args.yes_heldout:
        fail("comparing against heldout/ requires --yes-heldout (same discipline as recording)")

    out_dir = CORPUS_DIR / args.split / "winh"
    out_dir.mkdir(parents=True, exist_ok=True)
    ledger = Ledger()
    session_tag = utc_now()

    target = args.target or read_line("  Target application (e.g. 'VS Code 1.9x'): ").strip()
    winver = args.windows_version or read_line("  Windows build (winver): ").strip()
    say()
    say("  For each sentence: put the caret in the target, press Win+H, speak it once,")
    say("  stop, then copy the produced text and paste it here. Finish each paste with an")
    say("  empty line. Type '.skip' on the first line to skip a sentence.")

    ledger.append("winh_session_started", split=args.split, target=target,
                  windows_version=winver, session_tag=session_tag, count=len(prompts))

    recorded = 0
    for i, prompt in enumerate(prompts, 1):
        path = out_dir / f"{prompt.id}.json"
        if path.exists() and not args.redo:
            continue
        show_prompt(prompt, i, len(prompts))
        say()
        say("  Paste Win+H's output, then an empty line:")
        lines: list[str] = []
        while True:
            line = read_line("  | ")
            if line.strip() == ".skip" and not lines:
                lines = []
                break
            if not line.strip():
                break
            lines.append(line)
        if not lines:
            continue
        attempts = read_line("  attempts (Enter for 1): ").strip() or "1"
        note = read_line("  note (Enter for none): ").strip()

        artifact = {
            "schema": SIDECAR_SCHEMA,
            "record_py_version": RECORD_PY_VERSION,
            "kind": "winh_comparison",
            "prompt_id": prompt.id,
            "category": prompt.category,
            "split": prompt.split,
            "prompt_text": prompt.text,
            "prompt_terms": list(prompt.terms),
            "prompt_source_sha256": prompt.source_sha256,
            "target_app": target,
            "windows_version": winver,
            "attempts": int(attempts) if attempts.isdigit() else attempts,
            "hypothesis_raw": "\n".join(lines),
            "operator": args.operator or os.environ.get("USERNAME", ""),
            "operator_note": note,
            "captured_utc": utc_now(),
            "session_tag": session_tag,
        }
        path.write_text(json.dumps(artifact, indent=2, ensure_ascii=False), encoding="utf-8")
        ledger.append("winh_captured", prompt_id=prompt.id, split=prompt.split,
                      target=target, session_tag=session_tag,
                      path=str(path.relative_to(REPO_ROOT)).replace("\\", "/"),
                      sha256=sha256_file(path))
        recorded += 1

    ledger.append("winh_session_ended", split=args.split, session_tag=session_tag,
                  captured=recorded)
    say()
    say(f"  captured {recorded} Win+H comparisons into {out_dir}")
    return 0


# ── clean/ import ───────────────────────────────────────────────────────────────────────────


def cmd_import_clean(args: argparse.Namespace) -> int:
    """clean/ is a LibriSpeech slice used purely as an adapter sanity check (plan.md §3 Phase 1
    step 2). It is imported, never recorded: recording it here would defeat its only purpose,
    which is to be audio whose correct transcription is not in dispute.

    Expects a directory of `<id>.wav` alongside `<id>.txt` holding the reference transcript.
    Files are copied, hashed and ledgered exactly like a recorded take, so `verify` covers them
    on the same terms — but their sidecars are marked `"origin": "imported"`, because their
    audio did NOT come through this project's capture chain and must never be read as evidence
    about it.
    """
    src = Path(args.source)
    if not src.is_dir():
        fail(f"not a directory: {src}")
    if args.split == "heldout" and not args.yes_heldout:
        fail("importing into heldout/ requires --yes-heldout")

    dest_root = CORPUS_DIR / args.split / "clean"
    ledger = Ledger()
    session_tag = utc_now()
    imported = 0

    for wav in sorted(src.glob("*.wav")):
        ref = wav.with_suffix(".txt")
        if not ref.exists():
            warn(f"{wav.name}: no matching .txt reference — skipped")
            continue
        clip_id = f"clean-{wav.stem}"
        dest_wav = dest_root / f"{clip_id}.16k.wav"
        if dest_wav.exists() and not args.redo:
            continue
        dest_root.mkdir(parents=True, exist_ok=True)
        with contextlib.suppress(OSError):
            if dest_wav.exists():
                dest_wav.chmod(stat.S_IWRITE | stat.S_IREAD)
        shutil.copy2(wav, dest_wav)
        sidecar = {
            "schema": SIDECAR_SCHEMA,
            "record_py_version": RECORD_PY_VERSION,
            "origin": "imported",
            "driver": "none — imported, NOT captured through kd_record",
            "simulated": False,
            "prompt_id": clip_id,
            "category": "clean",
            "split": args.split,
            "prompt_text": ref.read_text(encoding="utf-8").strip(),
            "prompt_terms": [],
            "prompt_source": str(src),
            "take": 1,
            "recorded_utc": utc_now(),
            "source_path": str(wav),
            "files": {"wav_16k": dest_wav.name, "wav_native": None, "recorder_meta": None},
            "sha256": {"wav_16k": sha256_file(dest_wav), "wav_native": None},
            "duration_ms_measured": wav_duration_ms(dest_wav),
        }
        sidecar_path = dest_root / f"{clip_id}.16k.json"
        with contextlib.suppress(OSError):
            if sidecar_path.exists():
                sidecar_path.chmod(stat.S_IWRITE | stat.S_IREAD)
        sidecar_path.write_text(json.dumps(sidecar, indent=2, ensure_ascii=False),
                                encoding="utf-8")
        rel = str(dest_root.relative_to(REPO_ROOT)).replace("\\", "/")
        ledger.append("clip_imported", prompt_id=clip_id, split=args.split, category="clean",
                      take=1, session_tag=session_tag, path=rel,
                      files=sidecar["files"], sha256=sidecar["sha256"], source=str(wav))
        ledger.append("take_accepted", prompt_id=clip_id, split=args.split, take=1,
                      session_tag=session_tag, path=rel)
        if args.split == "heldout":
            set_read_only(dest_wav)
            set_read_only(sidecar_path)
        imported += 1

    say(f"  imported {imported} clean clips into {dest_root}")
    return 0


# ── verify / status ─────────────────────────────────────────────────────────────────────────


def cmd_verify(args: argparse.Namespace) -> int:
    """Recompute everything that the split discipline rests on.

    Three independent statements of a clip's split must agree: the directory it sits in, its
    sidecar, and the committed prompt file. Relocating a clip by hand breaks all three at once,
    which is the point — a single authority could be edited, three cannot be edited silently.
    """
    problems: list[str] = []
    ledger = Ledger()
    problems += ledger.verify_chain()

    registry = prompt_registry()
    ledger_ids: dict[str, dict] = {}
    for entry in ledger.entries():
        if entry.get("event") in ("take_recorded", "clip_imported"):
            ledger_ids[f"{entry.get('prompt_id')}::{entry.get('take')}"] = entry

    checked = 0
    simulated_in_corpus = 0

    for split in SPLITS:
        split_root = CORPUS_DIR / split
        if not split_root.exists():
            continue
        # Only the ACCEPTED clips — `<category>/<id>.16k.json`. Superseded takes under
        # `takes/` are checked separately below; they are history, not corpus.
        for sidecar_path in sorted(split_root.glob("*/*.16k.json")):
            checked += 1
            rel = sidecar_path.relative_to(REPO_ROOT).as_posix()
            try:
                data = json.loads(sidecar_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                problems.append(f"{rel}: unreadable sidecar ({exc})")
                continue

            # 0. the sidecar names the clip it sits beside. A sidecar whose id does not match
            #    its own filename means the pair was assembled by hand.
            if data.get("prompt_id") != sidecar_path.name[: -len(".16k.json")]:
                problems.append(
                    f"{rel}: sidecar prompt_id {data.get('prompt_id')!r} does not match its "
                    f"filename — the sidecar and the audio have been paired by hand")
            if data.get("category") != sidecar_path.parent.name:
                problems.append(
                    f"{rel}: sidecar category {data.get('category')!r} but it sits in "
                    f"{sidecar_path.parent.name}/")

            # 1. directory split vs sidecar split
            if data.get("split") != split:
                problems.append(
                    f"{rel}: sidecar says split={data.get('split')!r} but the file is under "
                    f"{split}/ — a clip has been moved between splits")

            # 2. sidecar split vs the COMMITTED prompt file
            pid = data.get("prompt_id", "")
            known = registry.get(pid)
            if known is None:
                if data.get("origin") != "imported":
                    problems.append(
                        f"{rel}: prompt id {pid!r} is not in bench/prompts/ — either the prompt "
                        f"was deleted or this clip was hand-made")
            elif known.split != split:
                problems.append(
                    f"{rel}: bench/prompts/ registers {pid!r} as {known.split}, but the clip is "
                    f"in {split}/ — the split has been changed without re-recording")
            elif known.source_sha256 != data.get("prompt_source_sha256"):
                problems.append(
                    f"{rel}: recorded against an older revision of "
                    f"{known.source_file.name}; the reference text may have changed since")

            # 3. file hashes
            for key, name in (("wav_16k", data.get("files", {}).get("wav_16k")),
                              ("wav_native", data.get("files", {}).get("wav_native"))):
                if not name:
                    continue
                audio = sidecar_path.parent / name
                if not audio.exists():
                    problems.append(f"{rel}: {name} is missing")
                    continue
                want = (data.get("sha256") or {}).get(key)
                if want and sha256_file(audio) != want:
                    problems.append(f"{rel}: {name} SHA-256 does not match its sidecar")

            # 4. the ledger recorded this exact take, into this exact split
            key = f"{pid}::{data.get('take')}"
            entry = ledger_ids.get(key)
            if entry is None:
                problems.append(
                    f"{rel}: no ledger entry for {key} — this clip was created outside "
                    f"record.py")
            elif entry.get("split") != split:
                problems.append(
                    f"{rel}: the ledger recorded {key} into {entry.get('split')}/, but it is "
                    f"now in {split}/ — the clip has been moved between splits")
            elif entry.get("sha256", {}).get("wav_16k") not in (
                    None, (data.get("sha256") or {}).get("wav_16k")):
                problems.append(
                    f"{rel}: the audio hash differs from what the ledger recorded — the file "
                    f"has been replaced since it was accepted")

            # 5. held-out hygiene
            if split == "heldout":
                for name in (data.get("files", {}).get("wav_16k"),
                             data.get("files", {}).get("wav_native")):
                    if not name:
                        continue
                    audio = sidecar_path.parent / name
                    if audio.exists() and not is_read_only(audio):
                        problems.append(f"{rel}: held-out file {name} is writable")

            if data.get("simulated"):
                simulated_in_corpus += 1
                problems.append(
                    f"{rel}: SIMULATED audio inside the real corpus — this clip contains a "
                    f"synthetic tone, not speech, and must not be measured")

    # 6. the harness reads `<split>/<category>/*.16k.wav` non-recursively. Anything else that
    #    matches that glob would be scored as an extra clip, so say so loudly.
    superseded = 0
    for split in SPLITS:
        for category_path in sorted((CORPUS_DIR / split).glob("*")):
            if not category_path.is_dir() or category_path.name == "winh":
                continue
            for wav in sorted(category_path.glob("*.16k.wav")):
                if not (wav.parent / f"{wav.name[:-len('.16k.wav')]}.16k.json").is_file():
                    problems.append(
                        f"{wav.relative_to(REPO_ROOT).as_posix()}: audio with no sidecar — "
                        f"run_matrix.py would score it against an empty reference")
            superseded += len(list((category_path / "takes").glob("*.16k.wav"))) \
                if (category_path / "takes").is_dir() else 0

    say(f"  checked {checked} accepted clip(s), {superseded} superseded take(s) in takes/, "
        f"{len(ledger.entries())} ledger entr(ies)")
    if simulated_in_corpus:
        say(f"  {simulated_in_corpus} simulated clip(s) found in dev/ or heldout/")
    if problems:
        say()
        for p in problems:
            say(f"  FAIL  {p}")
        say()
        say(f"  {len(problems)} problem(s).")
        return 1
    say("  OK — splits, hashes and ledger chain all agree.")
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    registry = prompt_registry()
    accepted = Ledger().accepted_takes()
    say()
    say(f"  {'category':<10} {'split':<9} {'recorded':>9} {'total':>7}")
    say(f"  {'-' * 10} {'-' * 9} {'-' * 9} {'-' * 7}")
    for cat in RECORDED_CATEGORIES:
        for split in SPLITS:
            ids = [p.id for p in registry.values()
                   if p.category == cat and p.split == split]
            if not ids:
                continue
            have = sum(1 for i in ids if i in accepted)
            say(f"  {cat:<10} {split:<9} {have:>9} {len(ids):>7}")
    say()
    # clean/ has no prompt file — it is imported, so the disk is the only authority on it.
    for split in SPLITS:
        clean_dir = CORPUS_DIR / split / "clean"
        n = len(list(clean_dir.glob("*.16k.wav"))) if clean_dir.is_dir() else 0
        say(f"  {'clean':<10} {split:<9} {n:>9} {'imported':>7}")
    say()
    for split in SPLITS:
        winh_dir = CORPUS_DIR / split / "winh"
        n = len(list(winh_dir.glob("*.json"))) if winh_dir.is_dir() else 0
        say(f"  Win+H comparisons ({split}): {n}")
    say(f"  corpus root: {CORPUS_DIR}  (gitignored — plan.md §1.9)")
    return 0


def cmd_devices(args: argparse.Namespace) -> int:
    driver = make_driver(args.recorder, args.simulate)
    devices = driver.devices()
    if args.json:
        say(json.dumps(devices, indent=2))
        return 0
    say()
    for d in devices:
        mark = "*" if d.get("is_default") else " "
        fmt = d.get("mix_format", {})
        say(f"  {mark} {d.get('friendly_name', '?')}")
        say(f"      id     {d.get('id', '?')}")
        say(f"      format {fmt.get('sample_rate', '?')} Hz / {fmt.get('channels', '?')} ch / "
            f"{fmt.get('format', '?')}")
    say()
    say("  '*' marks the default device. Pass --device <id> to pin one; a benchmark run")
    say("  should always pin, so a mid-session default change cannot alter the corpus.")
    return 0


# ── CLI ─────────────────────────────────────────────────────────────────────────────────────


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="record.py",
        description="Corpus recorder. Drives the C++ kd_record capture path; never opens a "
                    "microphone itself (plan.md §3 Phase 1).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="heldout/ is the decision set. It is recorded once and never tuned on.")
    sub = p.add_subparsers(dest="command", required=True)

    def add_recorder_args(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--recorder", help="path to kd_record.exe (else $KD_RECORDER, else "
                                           "the build directories)")
        sp.add_argument("--simulate", action="store_true",
                        help="bookkeeping only: synthetic audio into corpus/_simulated/. "
                             "NOT a recording mode.")

    sp = sub.add_parser("devices", help="list capture devices via kd_record")
    add_recorder_args(sp)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_devices)

    sp = sub.add_parser("record", help="prompt-driven recording session")
    add_recorder_args(sp)
    sp.add_argument("--split", choices=SPLITS, default="dev",
                    help="which pre-registered split to walk (default: dev)")
    sp.add_argument("--yes-heldout", action="store_true",
                    help="required, together with an interactive confirmation, for heldout/")
    sp.add_argument("--category", action="append", choices=list(RECORDED_CATEGORIES),
                    help="repeatable; default: all recordable categories")
    sp.add_argument("--only", action="append", metavar="ID",
                    help="repeatable; record only these prompt ids")
    sp.add_argument("--device", help="pin a capture device by endpoint id")
    sp.add_argument("--operator", help="who is speaking (recorded in every sidecar)")
    sp.add_argument("--notes", help="free-text session notes (room, mic, distance)")
    sp.add_argument("--redo", action="store_true",
                    help="also walk prompts that already have an accepted take")
    sp.add_argument("--max-seconds", type=float, default=DEFAULT_MAX_SECONDS,
                    help=f"per-take safety cap (default {DEFAULT_MAX_SECONDS:g})")
    sp.add_argument("--chunk-ms", type=int, default=20,
                    help="capture callback size; matches CaptureConfig::chunk_ms")
    sp.set_defaults(func=cmd_record)

    sp = sub.add_parser("winh", help="manual Win+H comparison (plan.md §4)")
    sp.add_argument("--split", choices=SPLITS, default="dev")
    sp.add_argument("--yes-heldout", action="store_true")
    sp.add_argument("--category", action="append", choices=["tech", "hard"])
    sp.add_argument("--only", action="append", metavar="ID")
    sp.add_argument("--target", help="target application, e.g. 'VS Code 1.9x'")
    sp.add_argument("--windows-version", help="output of winver")
    sp.add_argument("--operator")
    sp.add_argument("--redo", action="store_true")
    sp.set_defaults(func=cmd_winh)

    sp = sub.add_parser("import-clean", help="import a LibriSpeech slice into clean/")
    sp.add_argument("--source", required=True, help="directory of <id>.wav + <id>.txt")
    sp.add_argument("--split", choices=SPLITS, default="dev")
    sp.add_argument("--yes-heldout", action="store_true")
    sp.add_argument("--redo", action="store_true")
    sp.set_defaults(func=cmd_import_clean)

    sp = sub.add_parser("verify", help="check splits, hashes and the ledger chain")
    sp.set_defaults(func=cmd_verify)

    sp = sub.add_parser("status", help="what has been recorded so far")
    sp.set_defaults(func=cmd_status)

    return p


def main(argv: Sequence[str] | None = None) -> int:
    _reconfigure_stdio()
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except RecorderUnavailable as exc:
        fail(str(exc), code=3)
    except RecorderFailed as exc:
        fail(str(exc), code=4)


if __name__ == "__main__":
    raise SystemExit(main())
