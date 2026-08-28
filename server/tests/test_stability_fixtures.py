# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""The other half of the T2 gate: the oracle re-running its own recording.

``tests/fixtures/stability/`` is generated from :class:`kvit_asr.stability.StabilityTracker` by
``server/tools/gen_stability_fixtures.py``, and ``tests/test_asrd_stability.cpp`` asserts that
the C++ port produces the same numbers. That is the comparison openai-translator.md §5 asks for.

This file looks circular and is not, and the distinction is the reason it exists. The C++ side
compares itself against a *recording*, so a regression in the Python tracker would silently move
the baseline the next time anybody regenerates — and the two implementations would agree
perfectly on the wrong answer. Running the recording back through the oracle is what turns that
into a failure here instead.

If this fails, the Python tracker changed. Decide whether the change was intended before
regenerating: a regenerated fixture is a new contract, not a fix.
"""

from __future__ import annotations

import json
import os
import pathlib

import pytest

from kvit_asr.stability import (
    ManualClock,
    StabilityParams,
    StabilityTracker,
    StabilityViolation,
)


def _fixture_dir() -> pathlib.Path:
    # KD_FIXTURE_DIR is set by CMake for the C++ suite and by conftest for this one; the
    # repository layout is the fallback so a bare `pytest` in server/ still works.
    env = os.environ.get("KD_FIXTURE_DIR")
    if env:
        return pathlib.Path(env) / "stability"
    return pathlib.Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "stability"


FIXTURES = _fixture_dir()


def _index() -> dict:
    path = FIXTURES / "index.json"
    if not path.exists():
        pytest.skip(f"stability fixtures not found at {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def _case_ids() -> list[str]:
    try:
        return [c["id"] for c in _index()["cases"]]
    except Exception:  # noqa: BLE001 - collection must not explode on a missing corpus
        return []


def test_index_is_the_manifest() -> None:
    """Every listed case exists, and each file agrees with the index about its own id.

    Same rule docs/protocol.md §12 gives the protocol corpus: the index is the manifest, and a
    fixture that exists but is not listed must show up as a missing case rather than run
    silently.
    """
    index = _index()
    assert index["cases"], "the corpus is empty"
    for entry in index["cases"]:
        path = FIXTURES / entry["file"]
        assert path.exists(), f"{entry['id']} is listed but {path} is missing"
        doc = json.loads(path.read_text(encoding="utf-8"))
        assert doc["id"] == entry["id"]


@pytest.mark.parametrize("case_id", _case_ids())
def test_the_tracker_still_answers_what_was_recorded(case_id: str) -> None:
    doc = json.loads((FIXTURES / f"{case_id}.json").read_text(encoding="utf-8"))

    clock = ManualClock(0)
    tracker = StabilityTracker(StabilityParams(**doc["params"]), clock=clock)

    for i, step in enumerate(doc["steps"]):
        op = step["op"]
        where = f"{case_id} step[{i}] {op}"

        if op == "reset":
            tracker.reset(utterance_id=step.get("utterance_id", 0))
            continue
        if op == "advance":
            clock.advance(int(step["ms"]))
            continue
        if op == "stop_after_violation":
            # The program ends here by construction: §8.2 binds the next message against the
            # previously published mark, so a refused utterance has no conforming successor.
            break

        want_violation = step.get("expect_violation")
        try:
            if op == "observe":
                got = tracker.observe(step["text"])
            elif op == "poll":
                got = tracker.poll()
            elif op == "finalize":
                got = tracker.finalise(step["text"])
            else:
                raise AssertionError(f"{where}: unknown op")
        except StabilityViolation as v:
            assert want_violation is not None, f"{where}: unexpected {v.kind}"
            assert v.kind == want_violation, where
            if "expect_diverge_offset" in step:
                assert v.diverge_offset == step["expect_diverge_offset"], where
            continue

        assert want_violation is None, f"{where}: expected {want_violation}, got {got}"
        assert got == step["expect_stable_bytes"], where
        if "expect_high_water" in step:
            assert tracker.high_water == step["expect_high_water"], where


def test_every_published_mark_is_a_grapheme_boundary() -> None:
    """The invariant docs/protocol.md §8.2 requirement 2 states, over the whole corpus.

    Asserted separately from the recorded values because it must hold for *any* recording: a
    regenerated corpus that happened to bake in an off-boundary mark would still pass the
    parametrised test above.
    """
    from kvit_asr.stability import is_grapheme_boundary

    for entry in _index()["cases"]:
        doc = json.loads((FIXTURES / entry["file"]).read_text(encoding="utf-8"))
        last_text = ""
        for step in doc["steps"]:
            if step["op"] in ("observe", "finalize"):
                last_text = step["text"]
            mark = step.get("expect_stable_bytes")
            if mark is None:
                continue
            assert is_grapheme_boundary(last_text, mark), (
                f"{entry['id']}: recorded stable_bytes={mark} is not a cluster boundary of "
                f"{last_text!r}"
            )


def test_no_recorded_mark_ever_decreases_within_an_utterance() -> None:
    """§8.2 requirement 3, over the corpus. The other invariant a regeneration cannot break."""
    for entry in _index()["cases"]:
        doc = json.loads((FIXTURES / entry["file"]).read_text(encoding="utf-8"))
        high_water = 0
        for step in doc["steps"]:
            if step["op"] == "reset":
                high_water = 0
                continue
            mark = step.get("expect_stable_bytes")
            if mark is None:
                continue
            assert mark >= high_water, f"{entry['id']}: {mark} < {high_water}"
            high_water = mark
