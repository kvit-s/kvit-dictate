# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
# -*- coding: utf-8 -*-
"""Tests for bench/run_matrix.py.

The point of these tests is not coverage. It is that the four mechanisms plan.md §4 depends on —
the phase guard, the dev/heldout guard, resumability, and the pre-registered selection rule —
fail loudly when broken, because each of them is a mechanism whose failure mode is *silence*: a
sweep that quietly widened, a tuning run that quietly used the decision set, a cell that quietly
re-ran, a configuration that quietly vanished from the table.

Stdlib unittest, no third-party dependencies. Run with:
    C:/Python312/python.exe -m unittest discover -s bench/tests -t .
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
import wave
from pathlib import Path

# bench/ is not a package (bench/tests/ is), so import the module under test by path. This works
# under `unittest discover -t .` and when the file is run directly.
_BENCH_DIR = Path(__file__).resolve().parent.parent
if str(_BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(_BENCH_DIR))

import run_matrix as rm  # noqa: E402


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# helpers
# ═══════════════════════════════════════════════════════════════════════════════════════════════


def make_cell(**over) -> rm.Cell:
    base = dict(
        phase=1,
        model="nemotron-streaming-en-0.6b-onnx",
        category="tech",
        split="dev",
        context_mode="context_free",
        chunk_ms_requested=None,
        chunk_ms_actual=None,
        stability_n=2,
        stability_m=3,
        stability_t_ms=200,
        cadence="immediate",
        inject_method="SendInput",
        replay_speed=1.0,
        repeats=3,
    )
    base.update(over)
    return rm.Cell(**base)  # type: ignore[arg-type]


def synth_record(
    *,
    config_key: str,
    model: str = "m",
    category: str = "tech",
    context_mode: str = "context_free",
    split: str = "heldout",
    phase: int = 2,
    f1: float | None = 0.9,
    icer: float = 1.0,
    first_partial: float = 200.0,
    finalisation: float = 400.0,
    vram: float = 3000.0,
    synthetic: bool = False,
    chunk: int | None = 160,
    hallucination: float | None = None,
) -> dict:
    """A hand-built cell record. Deliberately not produced by the runner: the selection rule must
    be testable against numbers chosen to exercise it, not against whatever a mock happened to
    emit."""
    metrics: dict = {
        "icer_per_1000": icer,
        "first_partial_ms": first_partial,
        "finalisation_ms": finalisation,
        "peak_vram_mb": vram,
    }
    if f1 is not None:
        metrics["technical_term_f1"] = f1
    if hallucination is not None:
        metrics["hallucination_rate"] = hallucination
    return {
        "schema_version": rm.SCHEMA_VERSION,
        "cell_key": f"{config_key}:{category}",
        "configuration_key": config_key,
        "status": "ok",
        "synthetic": synthetic,
        "split": split,
        "config": {
            "phase": phase,
            "model": model,
            "category": category,
            "split": split,
            "context_mode": context_mode,
            "chunk_ms_requested": chunk,
            "chunk_ms_actual": chunk,
            "stability_n": 2,
            "stability_m": 3,
            "stability_t_ms": 200,
            "cadence": "immediate",
            "inject_method": "SendInput",
            "replay_speed": 1.0,
            "repeats": 3,
        },
        "metrics": metrics,
        "coverage_caps": [],
        "code_version": {"run_matrix_sha256_16": "deadbeefdeadbeef"},
    }


def write_wav(path: Path, ms: int = 200) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    n = int(rm.SAMPLE_RATE * ms / 1000)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rm.SAMPLE_RATE)
        wf.writeframes(b"\x00\x00" * n)


def make_corpus(root: Path, split: str = "dev") -> None:
    for cat in rm.CATEGORIES:
        clip = root / split / cat / "0001.16k.wav"
        write_wav(clip)
        if cat not in rm.HALLUCINATION_CATEGORIES:
            (clip.parent / "0001.16k.txt").write_text(
                "run llama.cpp with --n-gpu-layers 35", encoding="utf-8"
            )


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# cell keys
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestCellKeys(unittest.TestCase):
    def test_key_is_stable_and_filename_safe(self):
        a, b = make_cell(), make_cell()
        self.assertEqual(a.key, b.key)
        self.assertRegex(a.key, r"^[A-Za-z0-9._-]+$")

    def test_every_config_field_changes_the_key(self):
        base = make_cell()
        variants = [
            make_cell(phase=2),
            make_cell(model="streaming-zipformer-en-2023-06-26"),
            make_cell(category="hard"),
            make_cell(split="heldout"),
            make_cell(context_mode="context_enabled"),
            make_cell(chunk_ms_requested=80, chunk_ms_actual=80),
            make_cell(stability_n=3),
            make_cell(stability_m=5),
            make_cell(stability_t_ms=400),
            make_cell(cadence="500ms"),
            make_cell(inject_method="clipboard"),
            make_cell(replay_speed=4.0),
            make_cell(repeats=1),
        ]
        keys = {base.key} | {v.key for v in variants}
        self.assertEqual(len(keys), len(variants) + 1, "two configurations collided on one key")

    def test_configuration_key_ignores_category_only(self):
        # The selection rule scores configurations across categories, so tech and hard must share
        # a configuration key while a different chunk size must not.
        self.assertEqual(
            make_cell(category="tech").configuration_key,
            make_cell(category="hard").configuration_key,
        )
        self.assertNotEqual(
            make_cell(category="tech").configuration_key,
            make_cell(category="tech", chunk_ms_actual=80).configuration_key,
        )


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# phases
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestPhaseGuard(unittest.TestCase):
    def test_phase1_refuses_a_chunk_sweep(self):
        opts = rm.GridOptions(
            phase=1, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
            chunk_ms=(80, 160),
        )
        with self.assertRaises(rm.PhaseError) as ctx:
            rm.build_cells(opts)
        self.assertIn("chunk_ms", str(ctx.exception))

    def test_phase1_refuses_a_stability_sweep(self):
        opts = rm.GridOptions(
            phase=1, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
            stability_t_ms=(100, 200, 400),
        )
        with self.assertRaises(rm.PhaseError):
            rm.build_cells(opts)

    def test_phase1_allows_pinning_a_single_value(self):
        # Pinning is not sweeping: one chunk size in Phase 1 is a legitimate fixed condition.
        opts = rm.GridOptions(
            phase=1, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
            chunk_ms=(160,), categories=("tech",), context_modes=("context_free",),
        )
        cells, _ = rm.build_cells(opts)
        self.assertEqual(len(cells), 1)
        self.assertEqual(cells[0].chunk_ms_actual, 160)

    def test_phase2_allows_the_full_streaming_sweep(self):
        opts = rm.GridOptions(
            phase=2, split="dev", models=("nemotron-3.5-streaming-0.6b-onnx",),
            categories=("tech",), context_modes=("context_free",),
            chunk_ms=(80, 160), cadences=rm.CADENCES,
            stability_t_ms=(100, 200), inject_methods=rm.INJECT_METHODS,
        )
        cells, _ = rm.build_cells(opts)
        self.assertEqual(len(cells), 2 * 3 * 2 * 2)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# coverage caps
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestCoverageCaps(unittest.TestCase):
    def test_chunk_substitution_is_logged_and_recorded_in_the_cell(self):
        # plan.md §4 asks for 640 ms; the EN Nemotron repos publish 560 ms. Running 560 in a cell
        # labelled 640 without saying so is precisely the silent truncation the rule forbids.
        opts = rm.GridOptions(
            phase=2, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
            categories=("tech",), context_modes=("context_free",), chunk_ms=(640,),
        )
        cells, caps = rm.build_cells(opts)
        self.assertEqual(cells[0].chunk_ms_requested, 640)
        self.assertEqual(cells[0].chunk_ms_actual, 560)
        kinds = [c.kind for c in caps]
        self.assertIn("chunk_substituted", kinds)
        self.assertIn("640", "\n".join(c.detail for c in caps))

    def test_model_without_a_chunk_knob_logs_a_cap(self):
        opts = rm.GridOptions(
            phase=2, split="dev", models=("streaming-zipformer-en-2023-06-26",),
            categories=("tech",), context_modes=("context_free",), chunk_ms=(160,),
        )
        cells, caps = rm.build_cells(opts)
        self.assertIsNone(cells[0].chunk_ms_actual)
        self.assertIn("chunk_unsupported", [c.kind for c in caps])

    def test_context_free_only_backend_drops_the_context_enabled_arm_loudly(self):
        opts = rm.GridOptions(
            phase=1, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
            categories=("tech",),
        )
        cells, caps = rm.build_cells(opts)
        self.assertEqual([c.context_mode for c in cells], ["context_free"])
        detail = "\n".join(c.detail for c in caps)
        self.assertIn("supports_context=False", detail)

    def test_max_cells_truncation_is_logged(self):
        opts = rm.GridOptions(
            phase=2, split="dev", models=("nemotron-3.5-streaming-0.6b-onnx",),
            categories=("tech", "hard"), context_modes=("context_free",),
            chunk_ms=(80, 160, 320), max_cells=2,
        )
        cells, caps = rm.build_cells(opts)
        self.assertEqual(len(cells), 2)
        cap = next(c for c in caps if c.kind == "max_cells")
        self.assertIn("NOT run", cap.detail)

    def test_excluding_hallucination_categories_is_called_out_by_name(self):
        opts = rm.GridOptions(
            phase=1, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
            categories=("tech",),
        )
        _, caps = rm.build_cells(opts)
        detail = "\n".join(c.detail for c in caps if c.kind == "categories_excluded")
        self.assertIn("silence", detail)
        self.assertIn("hallucination", detail.lower())

    def test_caps_appear_in_the_rendered_summary(self):
        # A cap that exists only in a JSON field is a cap nobody reads.
        opts = rm.GridOptions(
            phase=2, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
            categories=("tech",), context_modes=("context_free",), chunk_ms=(640,),
            max_cells=1,
        )
        cells, caps = rm.build_cells(opts)
        text = rm.render_summary(
            phase=2, split="dev", cells=cells, ran=1, skipped=0, failed=0, caps=caps,
            policy=rm.ReplayPolicy(), executor_name="mock", synthetic=True,
        )
        self.assertIn("COVERAGE CAPS", text)
        self.assertIn("chunk_substituted", text)
        self.assertIn("SYNTHETIC RUN", text)

    def test_unknown_cap_kind_is_refused(self):
        caps = rm.CoverageLog()
        with self.assertRaises(rm.ConfigError):
            caps.add("mystery", "x", "y")

    def test_corpus_sampling_is_logged(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for i in range(5):
                p = root / "dev" / "tech" / f"{i:04d}.16k.wav"
                write_wav(p)
                (p.parent / f"{i:04d}.16k.txt").write_text("hello", encoding="utf-8")
            caps = rm.CoverageLog()
            clips = rm.discover_clips(root, "dev", "tech", max_clips=2, seed=1, caps=caps)
            self.assertEqual(len(clips), 2)
            cap = next(c for c in caps if c.kind == "corpus_sampling")
            self.assertIn("2 of 5", cap.detail)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# dev / heldout discipline
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestHeldoutGuard(unittest.TestCase):
    def test_tuning_run_against_heldout_is_refused(self):
        with self.assertRaises(rm.GuardError) as ctx:
            rm.guard_split("heldout", heldout_decision=False, purpose="tuning")
        msg = str(ctx.exception)
        self.assertIn("REFUSING", msg)
        self.assertIn("--heldout-decision", msg)

    def test_dev_is_never_guarded(self):
        rm.guard_split("dev", heldout_decision=False, purpose="tuning")  # must not raise

    def test_heldout_with_the_flag_is_allowed(self):
        rm.guard_split("heldout", heldout_decision=True, purpose="selection")

    def test_cli_run_refuses_heldout_without_the_flag(self):
        import contextlib
        import io

        err = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            with contextlib.redirect_stderr(err):
                code = rm.main(
                    [
                        "--results-root", str(Path(td) / "results"),
                        "--corpus-root", str(Path(td) / "corpus"),
                        "run", "--phase", "1", "--split", "heldout",
                        "--models", "nemotron-streaming-en-0.6b-onnx",
                    ]
                )
            # Nothing may have been written: the refusal happens before any corpus access.
            self.assertFalse((Path(td) / "results").exists())
        self.assertEqual(code, 2)
        self.assertIn("REFUSING to run against heldout/", err.getvalue())

    def test_cli_run_against_heldout_with_the_flag_records_the_ledger(self):
        import contextlib
        import io

        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            make_corpus(td / "corpus", split="heldout")
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = rm.main(
                    [
                        "--results-root", str(td / "results"),
                        "--corpus-root", str(td / "corpus"),
                        "run", "--phase", "1", "--split", "heldout",
                        "--heldout-decision", "--purpose", "phase-1 selection",
                        "--models", "nemotron-streaming-en-0.6b-onnx",
                        "--executor", "mock",
                    ]
                )
            self.assertEqual(code, 0)
            ledger = json.loads(
                (td / "results" / "heldout-ledger.aggregate.json").read_text(encoding="utf-8")
            )
            self.assertEqual(len(ledger["accesses"]), 1)
            self.assertEqual(ledger["accesses"][0]["purpose"], "phase-1 selection")

    def test_split_is_recorded_in_every_result_file(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            store = rm.ResultStore(root / "results")
            cell = make_cell(split="dev")
            path = store.write(
                cell,
                rm.CellOutcome(status="ok", metrics={"technical_term_f1": 0.5}, executor="mock"),
                policy=rm.ReplayPolicy(),
                clips=[],
                version={"run_matrix_sha256_16": "x"},
                purpose="tuning",
            )
            rec = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(rec["split"], "dev")
            self.assertEqual(rec["config"]["split"], "dev")

    def test_heldout_ledger_appends_rather_than_overwrites(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            cells = [make_cell(split="heldout")]
            rm.append_heldout_ledger(root, purpose="selection", cells=cells, code_version={})
            path = rm.append_heldout_ledger(
                root, purpose="selection again", cells=cells, code_version={}
            )
            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(len(data["accesses"]), 2)
            self.assertEqual(path.name, "heldout-ledger.aggregate.json")

    def test_corrupt_ledger_is_not_silently_replaced(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "heldout-ledger.aggregate.json").write_text("{not json", encoding="utf-8")
            with self.assertRaises(rm.ConfigError):
                rm.append_heldout_ledger(root, purpose="x", cells=[], code_version={})


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# real-time replay
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestReplay(unittest.TestCase):
    def test_frame_schedule_is_real_time_by_default(self):
        pcm_len = rm.SAMPLE_RATE * 2 * 1  # 1 second
        sched = rm.frame_schedule(pcm_len, rm.ReplayPolicy())
        self.assertEqual(len(sched), 50)  # 20 ms frames
        self.assertAlmostEqual(sched[-1][2], 980.0)

    def test_faster_replay_compresses_the_schedule_and_is_labelled(self):
        pcm_len = rm.SAMPLE_RATE * 2
        policy = rm.ReplayPolicy(speed=4.0)
        sched = rm.frame_schedule(pcm_len, policy)
        self.assertAlmostEqual(sched[-1][2], 245.0)
        self.assertFalse(policy.realtime)
        self.assertIn("NOT REAL TIME", policy.warning or "")
        self.assertIn("NOT REAL TIME", json.dumps(policy.to_json()))

    def test_cli_refuses_faster_than_realtime_without_the_flag(self):
        parser = rm.build_parser()
        args = parser.parse_args(["plan", "--phase", "1", "--speed", "8"])
        with self.assertRaises(rm.GuardError) as ctx:
            rm._policy_from_args(args)
        self.assertIn("flatters", str(ctx.exception))

    def test_cli_allows_faster_replay_when_asked_by_name(self):
        parser = rm.build_parser()
        args = parser.parse_args(
            ["plan", "--phase", "1", "--speed", "8", "--allow-faster-than-realtime"]
        )
        policy = rm._policy_from_args(args)
        self.assertEqual(policy.speed, 8.0)
        self.assertFalse(policy.realtime)

    def test_replay_warning_reaches_the_summary(self):
        text = rm.render_summary(
            phase=2, split="dev", cells=[], ran=0, skipped=0, failed=0,
            caps=rm.CoverageLog(), policy=rm.ReplayPolicy(speed=10.0),
            executor_name="mock", synthetic=False,
        )
        self.assertIn("NOT REAL TIME", text)

    def test_paced_replay_uses_absolute_scheduling(self):
        # Injected clock and sleep: the schedule must be absolute, so a slow send does not push
        # every later frame late (which would make a "real-time" run quietly not real time).
        clock = {"t": 0.0}
        sleeps: list[float] = []

        def now() -> float:
            return clock["t"]

        def sleep(d: float) -> None:
            sleeps.append(d)
            clock["t"] += d

        sent: list[float] = []

        def send(_frame: bytes, pos_ms: float) -> None:
            sent.append(pos_ms)
            clock["t"] += 0.005  # 5 ms of send cost per 20 ms frame

        pcm = b"\x00\x00" * (rm.SAMPLE_RATE // 10)  # 100 ms
        report = rm.paced_replay(pcm, rm.ReplayPolicy(), send, now=now, sleep=sleep)
        self.assertEqual(report.frames, 5)
        self.assertEqual(sent, [0.0, 20.0, 40.0, 60.0, 80.0])
        # 20 ms nominal minus 5 ms already spent sending: absolute scheduling absorbs the cost.
        self.assertAlmostEqual(sleeps[1], 0.015, places=6)
        # 5 x (15 ms sleep + 5 ms send) with the first frame due immediately -> 85 ms.
        self.assertAlmostEqual(report.total_ms, 85.0, places=3)

    def test_read_wav_refuses_a_wrong_sample_rate(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "a.wav"
            with wave.open(str(p), "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(48000)
                wf.writeframes(b"\x00\x00" * 100)
            with self.assertRaises(rm.ConfigError) as ctx:
                rm.read_wav_pcm(p)
            self.assertIn("does not resample", str(ctx.exception))


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# resumability
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestResumability(unittest.TestCase):
    def _run(self, td: Path, **kw):
        cells, caps = rm.build_cells(
            rm.GridOptions(
                phase=1, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
                categories=("tech",), context_modes=("context_free",),
            )
        )
        opts = rm.RunOptions(
            results_root=td / "results", corpus_root=td / "corpus", **kw
        )
        return rm.run_cells(
            cells, executor=rm.MockExecutor(seed=1), policy=rm.ReplayPolicy(),
            grid_caps=caps, opts=opts, log=lambda s: None,
        )

    def test_completed_cell_is_skipped_and_forced_rerun_is_not(self):
        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            make_corpus(td / "corpus")
            first = self._run(td)
            self.assertEqual((first["ran"], first["skipped"]), (1, 0))

            second = self._run(td)
            self.assertEqual((second["ran"], second["skipped"]), (0, 1))

            forced = self._run(td, force=True)
            self.assertEqual((forced["ran"], forced["skipped"]), (1, 0))

    def test_failed_cell_is_retried_on_the_next_run(self):
        # A failure must not look like a completed cell, or a transient error would silently
        # remove a configuration from the table forever.
        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            # No corpus at all -> the cell fails.
            first = self._run(td)
            self.assertEqual(first["failed"], 1)
            make_corpus(td / "corpus")
            second = self._run(td)
            self.assertEqual((second["ran"], second["skipped"], second["failed"]), (1, 0, 0))

    def test_no_retry_is_logged_as_a_coverage_cap(self):
        class Boom(rm.Executor):
            name = "boom"
            synthetic = True

            def run_cell(self, cell, clips, policy):
                raise RuntimeError("backend exploded")

        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            make_corpus(td / "corpus")
            cells, caps = rm.build_cells(
                rm.GridOptions(
                    phase=1, split="dev", models=("nemotron-streaming-en-0.6b-onnx",),
                    categories=("tech",), context_modes=("context_free",),
                )
            )
            res = rm.run_cells(
                cells, executor=Boom(), policy=rm.ReplayPolicy(), grid_caps=caps,
                opts=rm.RunOptions(results_root=td / "r", corpus_root=td / "corpus"),
                log=lambda s: None,
            )
            kinds = [c.kind for c in res["caps"]]
            self.assertIn("cell_failed", kinds)
            self.assertIn("no_retry", kinds)

    def test_result_file_carries_the_full_config_and_code_version(self):
        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            make_corpus(td / "corpus")
            self._run(td)
            files = sorted((td / "results" / "cells").glob("*.json"))
            self.assertEqual(len(files), 1)
            rec = json.loads(files[0].read_text(encoding="utf-8"))
            for key in ("config", "config_digest", "code_version", "replay", "split", "synthetic"):
                self.assertIn(key, rec)
            self.assertEqual(set(rec["config"]), set(make_cell().config()))
            self.assertTrue(rec["synthetic"], "mock results must be stamped synthetic")

    def test_corrupt_result_file_is_reported_not_ignored(self):
        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            store = rm.ResultStore(td / "results")
            cell = make_cell()
            path = store.path_for(cell)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("{truncated", encoding="utf-8")
            with self.assertRaises(rm.ConfigError):
                store.load(cell)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# the selection rule
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestSelectionRule(unittest.TestCase):
    def _select(self, records, **kw):
        aggs = rm.aggregate_results(records)
        return rm.apply_selection_rule(
            aggs, kw.pop("thresholds", rm.Thresholds()),
            context_mode=kw.pop("context_mode", "context_free"),
            split=kw.pop("split", "heldout"),
            allow_synthetic=kw.pop("allow_synthetic", True),
        )

    def test_highest_f1_wins_when_every_gate_passes(self):
        recs = [
            synth_record(config_key="A", f1=0.80),
            synth_record(config_key="B", f1=0.91),
            synth_record(config_key="C", f1=0.85),
        ]
        out = self._select(recs)
        self.assertIsNotNone(out.winner)
        self.assertEqual(out.winner.configuration_key, "B")
        self.assertEqual([a.configuration_key for a in out.ranked], ["B", "C", "A"])

    def test_highest_f1_eliminated_by_icer_gives_the_runner_up_and_says_so(self):
        # The case the whole apparatus exists for: the best-looking configuration corrupts the
        # document more often than the safety threshold allows.
        recs = [
            synth_record(config_key="best_but_unsafe", f1=0.95, icer=9.0),
            synth_record(config_key="runner_up", f1=0.88, icer=0.4),
        ]
        out = self._select(recs)
        self.assertEqual(out.winner.configuration_key, "runner_up")
        elim = {e.configuration_key: e for e in out.eliminated}
        self.assertIn("best_but_unsafe", elim)
        self.assertTrue(any("icer" in r for r in elim["best_but_unsafe"].reasons))
        joined = " ".join(out.notes)
        self.assertIn("RUNNER-UP SELECTED", joined)
        self.assertIn("0.9500", joined)
        report = rm.render_selection_report([out])
        self.assertIn("RUNNER-UP SELECTED", report)
        self.assertIn("ELIMINATED", report)
        self.assertIn("best_but_unsafe", report)

    def test_each_gate_eliminates_and_names_itself(self):
        th = rm.Thresholds()
        cases = {
            "slow_first_partial": dict(first_partial=900.0),
            "slow_finalisation": dict(finalisation=1500.0),
            "unsafe": dict(icer=50.0),
            "fat": dict(vram=99999.0),
        }
        for key, kwargs in cases.items():
            with self.subTest(gate=key):
                out = self._select([synth_record(config_key=key, f1=0.99, **kwargs)], thresholds=th)
                self.assertIsNone(out.winner)
                reasons = " ".join(out.eliminated[0].reasons)
                self.assertTrue(reasons.strip(), "an elimination must carry a reason")

    def test_missing_metric_is_an_elimination_not_a_silent_drop(self):
        # An untested configuration must never be indistinguishable from one that lost.
        recs = [synth_record(config_key="incomplete", f1=None)]
        out = self._select(recs)
        self.assertIsNone(out.winner)
        self.assertEqual(len(out.eliminated), 1)
        self.assertTrue(
            any("missing_metric:technical_term_f1" in r for r in out.eliminated[0].reasons)
        )

    def test_ties_break_on_icer_then_latency(self):
        recs = [
            synth_record(config_key="tie_slow", f1=0.900, icer=0.5, first_partial=280.0),
            synth_record(config_key="tie_fast", f1=0.9005, icer=0.5, first_partial=120.0),
            synth_record(config_key="tie_safe", f1=0.901, icer=0.1, first_partial=290.0),
        ]
        out = self._select(recs)
        self.assertEqual(out.winner.configuration_key, "tie_safe")
        self.assertIn("tied on technical-term F1", " ".join(out.notes))
        # Remove the low-ICER entry: the tie then falls through to latency.
        out2 = self._select(recs[:2])
        self.assertEqual(out2.winner.configuration_key, "tie_fast")

    def test_context_modes_are_never_pooled(self):
        recs = [
            synth_record(config_key="free_low", f1=0.70, context_mode="context_free"),
            synth_record(config_key="ctx_high", f1=0.95, context_mode="context_enabled"),
        ]
        free = self._select(recs, context_mode="context_free")
        ctx = self._select(recs, context_mode="context_enabled")
        self.assertEqual(free.winner.configuration_key, "free_low")
        self.assertEqual(ctx.winner.configuration_key, "ctx_high")
        # The context-enabled entrant must not appear anywhere in the context-free decision.
        self.assertNotIn("ctx_high", [a.configuration_key for a in free.ranked])
        self.assertNotIn("ctx_high", [e.configuration_key for e in free.eliminated])

    def test_synthetic_results_are_refused_by_default(self):
        recs = [synth_record(config_key="mocked", f1=0.99, synthetic=True)]
        out = self._select(recs, allow_synthetic=False)
        self.assertIsNone(out.winner)
        self.assertTrue(any("synthetic" in r for e in out.eliminated for r in e.reasons))

    def test_dev_split_selection_is_labelled_not_a_decision(self):
        recs = [synth_record(config_key="A", f1=0.9, split="dev")]
        out = self._select(recs, split="dev")
        self.assertIn("NOT A DECISION", " ".join(out.notes))

    def test_no_winner_is_stated_rather_than_manufactured(self):
        recs = [synth_record(config_key="A", f1=0.9, icer=99.0)]
        out = self._select(recs)
        self.assertIsNone(out.winner)
        self.assertIn("NO WINNER", " ".join(out.notes))
        self.assertIn("does not permit relaxing a threshold", " ".join(out.notes))

    def test_an_empty_arm_reads_differently_from_a_defeated_one(self):
        # The context-enabled arm is routinely empty (CANDIDATES.md §5.1). "Nobody entered" and
        # "everybody lost" are different facts and must not print the same sentence.
        out = self._select([], context_mode="context_enabled")
        self.assertIsNone(out.winner)
        joined = " ".join(out.notes)
        self.assertIn("NO CONFIGURATIONS", joined)
        self.assertNotIn("NO WINNER", joined)

    def test_f1_comes_from_the_tech_category_only(self):
        # A 'clean' LibriSpeech slice has no technical vocabulary; letting it into the average
        # would dilute the quantity the rule maximises.
        recs = [
            synth_record(config_key="A", category="tech", f1=0.90),
            synth_record(config_key="A", category="clean", f1=0.10),
        ]
        aggs = rm.aggregate_results(recs)
        self.assertAlmostEqual(aggs[0].metrics["technical_term_f1"], 0.90)

    def test_icer_pools_by_its_true_denominator_when_counts_exist(self):
        a = synth_record(config_key="A", category="tech", icer=0.0)
        a["metrics"].update(icer_error_graphemes=1, icer_committed_graphemes=1000)
        b = synth_record(config_key="A", category="hard", icer=0.0)
        b["metrics"].update(icer_error_graphemes=9, icer_committed_graphemes=1000)
        agg = rm.aggregate_results([a, b])[0]
        self.assertAlmostEqual(agg.metrics["icer_per_1000"], 5.0)
        self.assertEqual(agg.metrics["icer_committed_graphemes"], 2000.0)

    def test_extra_hallucination_gate_is_labelled_as_not_pre_registered(self):
        recs = [
            synth_record(config_key="A", category="tech", f1=0.9),
            synth_record(config_key="A", category="silence", f1=None, hallucination=0.5),
        ]
        th = rm.Thresholds(max_hallucination_rate=0.1)
        out = self._select(recs, thresholds=th)
        self.assertIsNone(out.winner)
        self.assertTrue(any("extra_gate" in r for e in out.eliminated for r in e.reasons))

    def test_metrics_module_spelling_of_the_objective_is_accepted(self):
        # bench/metrics.py spells the objective `term_f1`. Neither module may quietly rename the
        # other's metric; the runner accepts the known spellings and keeps one canonical name.
        rec = synth_record(config_key="A", f1=None)
        rec["metrics"]["term_f1"] = 0.87
        agg = rm.aggregate_results([rec])[0]
        self.assertAlmostEqual(agg.metrics["technical_term_f1"], 0.87)
        self.assertNotIn("technical_term_f1", agg.missing)

    def test_gate_metrics_agree_with_the_metrics_module(self):
        # If bench/metrics.py starts nominating a gate metric this runner cannot read, that must
        # surface as a problem rather than as a gate quietly not being applied.
        problems = rm.check_gate_metrics_agree()
        for p in problems:
            self.assertNotIn("does not know how to read", p, msg=p)

    def test_thresholds_declare_their_provenance(self):
        src = rm.Thresholds().sources()
        self.assertIn("PROJECT-DECLARED", src["icer_per_1000"])
        self.assertIn("plan.md", src["first_partial_ms"])
        report = rm.render_selection_report([self._select([synth_record(config_key="A")])])
        self.assertIn("ICER<=", report)


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# commit simulation — cadence is flush timing, never eligibility
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestCommitSimulator(unittest.TestCase):
    def _sim(self, cadence="immediate"):
        return rm.CommitSimulator(cadence, grapheme_counter=len, grapheme_source="test:len")

    def test_only_stable_bytes_are_committed(self):
        sim = self._sim()
        sim.feed(rm.HypothesisEvent(0.0, 1, "hello world", stable_bytes=6))
        trace = sim.trace
        self.assertEqual([c.text for c in trace.commits], ["hello "])

    def test_cadence_delays_but_never_promotes(self):
        immediate = self._sim("immediate")
        immediate.feed(rm.HypothesisEvent(0.0, 1, "hello world", 6))
        immediate.feed(rm.HypothesisEvent(100.0, 1, "hello world today", 12))
        self.assertEqual(len(immediate.trace.commits), 2)

        slow = self._sim("500ms")
        slow.feed(rm.HypothesisEvent(0.0, 1, "hello world", 6))
        slow.feed(rm.HypothesisEvent(100.0, 1, "hello world today", 12))
        # The second flush is not due yet: cadence delayed it. It committed no *more* text, and
        # crucially it committed nothing unstable.
        self.assertEqual(len(slow.trace.commits), 1)
        slow.feed(rm.HypothesisEvent(700.0, 1, "hello world today", 12))
        self.assertEqual(len(slow.trace.commits), 2)
        self.assertEqual("".join(c.text for c in slow.trace.commits), "hello world ")

    def test_icer_counts_a_committed_prefix_the_final_contradicts(self):
        sim = self._sim()
        sim.feed(rm.HypothesisEvent(0.0, 1, "llama dot cpp", stable_bytes=6))  # commits "llama "
        sim.feed(rm.HypothesisEvent(200.0, 1, "llamas are fine", stable_bytes=15, is_final=True))
        trace = sim.finish()
        self.assertGreater(trace.irrevocable_error_graphemes, 0)
        self.assertGreater(trace.icer_per_1000(), 0.0)
        self.assertTrue(any("diverges" in v for v in trace.violations))

    def test_nothing_more_is_committed_after_a_divergence(self):
        # plan.md §1.4: preserve the document. Once a hypothesis contradicts committed text, the
        # engine may not repair it and may not keep appending as if nothing happened.
        sim = self._sim()
        sim.feed(rm.HypothesisEvent(0.0, 1, "llama dot cpp", 6))
        before = len(sim.trace.commits)
        sim.feed(rm.HypothesisEvent(100.0, 1, "llamas are fine and long", 24))
        sim.feed(rm.HypothesisEvent(200.0, 1, "llamas are fine and longer", 26, is_final=True))
        self.assertEqual(len(sim.trace.commits), before)

    def test_zero_commit_trace_does_not_divide_by_zero(self):
        trace = self._sim().finish()
        self.assertEqual(trace.committed_graphemes, 0)
        self.assertEqual(trace.icer_per_1000(), 0.0)

    def test_backwards_stable_bytes_is_recorded_as_a_violation(self):
        sim = self._sim()
        sim.feed(rm.HypothesisEvent(0.0, 1, "hello world", 11))
        sim.feed(rm.HypothesisEvent(50.0, 1, "hello world", 6))
        self.assertTrue(any("backwards" in v for v in sim.trace.violations))

    def test_final_flushes_regardless_of_cadence(self):
        sim = self._sim("1000ms")
        sim.feed(rm.HypothesisEvent(0.0, 1, "one two", 4))
        sim.feed(rm.HypothesisEvent(10.0, 1, "one two three", 13, is_final=True))
        self.assertEqual("".join(c.text for c in sim.trace.commits), "one two three")
        self.assertEqual(sim.finish().irrevocable_error_graphemes, 0)

    def test_utterance_boundary_resets_the_commit_state(self):
        sim = self._sim()
        sim.feed(rm.HypothesisEvent(0.0, 1, "first", 5, is_final=True))
        sim.feed(rm.HypothesisEvent(100.0, 2, "second", 6, is_final=True))
        self.assertEqual([c.utterance_id for c in sim.trace.commits], [1, 2])
        self.assertEqual([c.byte_start for c in sim.trace.commits], [0, 0])

    def test_stable_bytes_splitting_a_utf8_sequence_is_refused_not_injected(self):
        sim = self._sim()
        # "é" is two UTF-8 bytes; a mark of 2 lands inside it.
        sim.feed(rm.HypothesisEvent(0.0, 1, "aé", stable_bytes=2))
        self.assertEqual(sim.trace.commits, [])
        self.assertTrue(any("split a UTF-8 sequence" in v for v in sim.trace.violations))


# ═══════════════════════════════════════════════════════════════════════════════════════════════
# aggregates, privacy, and the end-to-end CLI
# ═══════════════════════════════════════════════════════════════════════════════════════════════


class TestAggregateHygiene(unittest.TestCase):
    def test_verbatim_keys_are_refused_in_a_committed_aggregate(self):
        with self.assertRaises(rm.ConfigError) as ctx:
            rm.assert_no_verbatim({"a": [{"hypotheses": ["secret"]}]}, where="test")
        self.assertIn("plan.md §1.9", str(ctx.exception))

    def test_clean_aggregate_passes(self):
        rm.assert_no_verbatim({"metrics": {"wer_raw": 0.1}}, where="test")


class TestEndToEndCli(unittest.TestCase):
    def test_mock_run_writes_cells_and_an_aggregate_and_prints_the_rule(self):
        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            make_corpus(td / "corpus")
            import io
            import contextlib

            buf = io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
                code = rm.main(
                    [
                        "--results-root", str(td / "results"),
                        "--corpus-root", str(td / "corpus"),
                        "run", "--phase", "1", "--split", "dev",
                        "--models", "nemotron-streaming-en-0.6b-onnx",
                        "--executor", "mock", "--repeats", "3",
                    ]
                )
            out = buf.getvalue()
            self.assertEqual(code, 0)
            self.assertTrue((td / "results" / "phase1.aggregate.json").is_file())
            cells = list((td / "results" / "cells").glob("*.json"))
            self.assertEqual(len(cells), len(rm.CATEGORIES))
            self.assertIn("COVERAGE CAPS", out)
            self.assertIn("SELECTION RULE", out)
            self.assertIn("SYNTHETIC", out)
            agg = json.loads((td / "results" / "phase1.aggregate.json").read_text(encoding="utf-8"))
            self.assertTrue(agg["any_synthetic"])
            self.assertEqual(agg["split"], "dev")
            self.assertIn("thresholds", agg)
            self.assertIn("coverage_caps", agg)

    def test_plan_command_runs_nothing(self):
        with tempfile.TemporaryDirectory() as tdname:
            td = Path(tdname)
            import io
            import contextlib

            buf = io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
                code = rm.main(
                    ["--results-root", str(td / "results"), "plan", "--phase", "1"]
                )
            self.assertEqual(code, 0)
            self.assertFalse((td / "results").exists())

    def test_sidecar_executor_fails_loudly_and_specifically(self):
        ex = rm.SidecarExecutor(python_exe=sys.executable)
        with self.assertRaises(rm.ExecutorError) as ctx:
            ex.run_cell(make_cell(), [], rm.ReplayPolicy())
        msg = str(ctx.exception)
        self.assertIn("envs/", msg)
        self.assertIn("--executor mock", msg)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
