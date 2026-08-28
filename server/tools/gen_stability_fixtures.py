# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Generate ``tests/fixtures/stability/`` by **recording** what the Python tracker does.

openai-translator.md §5 makes the C++/Python agreement on ``stable_bytes`` the T2 gate, and §8
names "porting stability.py introduces a divergence the fixtures do not catch" as a risk. The
protocol corpus cannot catch it: docs/protocol.md constrains the *sequence* of marks a Server
may emit, not which mark plan.md §1.4 says it should choose. Two conforming servers can publish
different numbers for the same hypotheses and both pass every one of the 21 cases.

So this file exists, and the shape of it matters:

**It records, it does not assert.** Every ``expect`` value in the generated fixtures is whatever
:class:`kvit_asr.stability.StabilityTracker` actually produced, not what the author of the case
believed it should produce. That is the whole point — the Python tracker is the oracle
(``src/asrd/stability.h`` says so), and a fixture written from an expectation would test the
expectation instead of the port. A case whose recorded value looks wrong is a finding about
plan.md §1.4 to be argued in the open, never a number to be edited here.

Both suites then run the same file: ``tests/test_asrd_stability.cpp`` and
``server/tests/test_stability_fixtures.py``. The Python side re-running its own recording is not
circular — it is what catches a *regression* in the oracle, which would otherwise silently
re-baseline the C++ side on the next regeneration.

Run it after any deliberate change to the algorithm, and commit the diff::

    cd server && uv run python tools/gen_stability_fixtures.py
"""

from __future__ import annotations

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from kvit_asr.stability import (  # noqa: E402
    ManualClock,
    StabilityParams,
    StabilityTracker,
    StabilityViolation,
)

REPO = pathlib.Path(__file__).resolve().parents[2]
OUT = REPO / "tests" / "fixtures" / "stability"

DEFAULTS = {"hypotheses_n": 2, "retract_clusters_m": 3, "hold_ms_t": 200}


def case(cid, name, description, steps, params=None, tags=()):
    return {
        "id": cid,
        "name": name,
        "description": description,
        "params": {**DEFAULTS, **(params or {})},
        "steps": steps,
        "tags": list(tags),
    }


# ─── The cases ────────────────────────────────────────────────────────────────────────────────
#
# `steps` is an ordered program. Each step is one of:
#
#   {"op": "reset",    "utterance_id": n}
#   {"op": "advance",  "ms": n}                 move the manual clock
#   {"op": "observe",  "text": "..."}           one new cumulative hypothesis
#   {"op": "poll"}                              re-evaluate the dwell, no new evidence
#   {"op": "finalize", "text": "..."}           close the utterance
#
# The generator fills in `expect_stable_bytes`, or `expect_violation` when the tracker refused.

CASES = [
    case(
        "001-first-hypothesis-publishes-nothing",
        "A single hypothesis is never stable",
        "plan.md §1.4 step 1 keeps the last N hypotheses and N defaults to 2. One hypothesis "
        "has not been confirmed by a second opinion, and publishing it is the exact failure the "
        "algorithm exists to prevent.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "can you change the"},
            {"op": "advance", "ms": 1000},
            {"op": "poll"},
        ],
        tags=("step1",),
    ),
    case(
        "002-dwell-must-mature",
        "Two agreeing hypotheses still wait T milliseconds",
        "Step 5. With N=2 the first real candidate comes into existence at the moment the "
        "second hypothesis lands, having then been unchanged for 0 ms — so observe(a); "
        "observe(b) publishes nothing however much time passed between a and b. The time passed "
        "before there was a candidate to hold. A poll T ms later is what matures it.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "hello world and more"},
            {"op": "advance", "ms": 5000},
            {"op": "observe", "text": "hello world and more"},
            {"op": "poll"},
            {"op": "advance", "ms": 199},
            {"op": "poll"},
            {"op": "advance", "ms": 1},
            {"op": "poll"},
        ],
        tags=("step5", "dwell"),
    ),
    case(
        "003-head-cap-is-clusters-behind-the-head",
        "Step 3 caps the candidate at M clusters behind the decode head",
        "The reading that had two readings. stability.py:831-836 flags it and takes the cap; "
        "subtracting M from the common prefix instead would be strictly more conservative, so "
        "the two differ in latency rather than in safety — but they differ, and both "
        "implementations must choose the same one. A test that disagrees here is a failed port, "
        "not a refinement.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "alpha beta gamma delta"},
            {"op": "observe", "text": "alpha beta gamma delta"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("step3",),
    ),
    case(
        "004-word-boundary-and-trailing-space",
        "Step 4 retracts to a word boundary and holds trailing whitespace back",
        "Never stop inside a word — a revised word ending is the most common revision there is "
        "— and hold the space back so a following punctuation revision does not require "
        "rewriting across a committed space.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "the quick brown fox jumps "},
            {"op": "observe", "text": "the quick brown fox jumps "},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("step4",),
    ),
    case(
        "005-common-prefix-over-diverging-hypotheses",
        "Only the prefix the last N hypotheses agree on is a candidate",
        "Step 2, on grapheme clusters rather than bytes. The divergence at the tail is where "
        "revision concentrates and is exactly what N exists to notice.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "the meeting is on tuesday afternoon"},
            {"op": "observe", "text": "the meeting is on thursday afternoon"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "observe", "text": "the meeting is on thursday afternoon"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("step2",),
    ),
    case(
        "006-mark-never-decreases",
        "A shrinking candidate does not shrink the published mark",
        "Step 6. The high-water mark is monotonic even when the hypothesis stops supporting it, "
        "because the Client may already have typed those bytes into the user's document.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "one two three four five"},
            {"op": "observe", "text": "one two three four five"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "observe", "text": "one two three four"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("step6", "monotonic"),
    ),
    case(
        "007-prefix-mutation-is-refused",
        "A hypothesis that contradicts the published prefix is refused, not clamped",
        "docs/protocol.md §8.3 forbids clamping because it hides the defect ICER exists to "
        "measure — and in this direction it would additionally mean shipping a message whose "
        "bytes below the published mark have changed, which is the failure that corrupts the "
        "user's document. This is the signature of a batch model that re-decodes rather than "
        "extends: diverge_offset 0 means it revised the first word.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "so i was thinking we should"},
            {"op": "observe", "text": "so i was thinking we should"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "observe", "text": "So I was thinking, we should"},
        ],
        tags=("violation", "batch-model"),
    ),
    case(
        "008-finalize-is-the-whole-text",
        "A final is entirely stable by definition",
        "docs/protocol.md §6.2. §8.2's last sentence still applies and is checked rather than "
        "assumed: utf8(final.text) MUST begin with the highest prefix already published.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "change the llama.cpp config"},
            {"op": "observe", "text": "change the llama.cpp config"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "finalize", "text": "change the llama.cpp config file."},
        ],
        tags=("final",),
    ),
    case(
        "009-finalize-contradicting-the-mark-is-refused",
        "A final that does not extend the published prefix is refused",
        "The last line of defence, on the message type where a violation is least recoverable: "
        "a `final` is what the Client commits.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "hello there friend"},
            {"op": "observe", "text": "hello there friend"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "finalize", "text": "goodbye there friend"},
        ],
        tags=("violation", "final"),
    ),
    case(
        "010-reset-forgets-everything",
        "An utterance boundary resets the mark, the window and the dwell",
        "plan.md §1.4 step 7. Entirely means entirely: carrying any of it across a boundary "
        "would let a mark established for utterance n constrain utterance n+1, which §8.2 does "
        "not do. The second utterance here would be a violation if anything survived.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "first utterance text here"},
            {"op": "observe", "text": "first utterance text here"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "reset", "utterance_id": 2},
            {"op": "observe", "text": "completely different words now"},
            {"op": "observe", "text": "completely different words now"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("step7", "reset"),
    ),
    case(
        "011-combining-marks-are-not-split",
        "The candidate lands on a grapheme-cluster boundary, not a byte one",
        "Two hypotheses that differ only in a combining mark share a byte prefix that ends "
        "*inside* a cluster. Committing it would type a base letter whose accent the model is "
        "still revising, and the Client would then have to repair a document it is forbidden to "
        "repair.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "café au lait please now"},
            {"op": "observe", "text": "café au lait please now"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("unicode", "grapheme"),
    ),
    case(
        "012-emoji-zwj-sequence-is-one-cluster",
        "A ZWJ sequence is one grapheme cluster and is never cut",
        "The family emoji is four code points and eleven bytes joined by ZWJ. A byte-wise or "
        "code-point-wise prefix would put half a family in the user's document.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "send the \U0001F468‍\U0001F469‍\U0001F467 emoji to everyone"},
            {"op": "observe", "text": "send the \U0001F468‍\U0001F469‍\U0001F467 emoji to everyone"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("unicode", "grapheme", "emoji"),
    ),
    case(
        "013-growing-hypothesis-restarts-the-dwell",
        "A candidate that advances on every hypothesis never matures",
        "The silent failure StabilityTracker.poll's docstring warns about, recorded as a "
        "fixture: while somebody is still talking the candidate moves on almost every "
        "hypothesis, each move restarts the hold, and a loop that publishes only on observe() "
        "emits stable_bytes = 0 for ever. Every message is well formed and the invariant is "
        "trivially satisfied by never committing anything.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "the first"},
            {"op": "advance", "ms": 300},
            {"op": "observe", "text": "the first part"},
            {"op": "advance", "ms": 300},
            {"op": "observe", "text": "the first part of"},
            {"op": "advance", "ms": 300},
            {"op": "observe", "text": "the first part of the"},
            {"op": "advance", "ms": 300},
            {"op": "observe", "text": "the first part of the sentence"},
        ],
        tags=("step5", "dwell"),
    ),
    case(
        "014-n3-needs-a-third-opinion",
        "N=3 publishes nothing until three hypotheses agree",
        "One of the sweep points plan.md §4 measures. The knob has to mean the same thing on "
        "both sides or the sweep compares two different algorithms.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "alpha bravo charlie delta echo"},
            {"op": "observe", "text": "alpha bravo charlie delta echo"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "observe", "text": "alpha bravo charlie delta echo"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        params={"hypotheses_n": 3},
        tags=("params",),
    ),
    case(
        "015-m0-and-t0-publish-immediately",
        "M=0, T=0 is the fastest legal setting and is still conforming",
        "The corner of the sweep with no retraction and no hold. It must still land on a word "
        "boundary and still never decrease — the two rules that are not tunable.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "one two three"},
            {"op": "observe", "text": "one two three"},
            {"op": "poll"},
        ],
        params={"retract_clusters_m": 0, "hold_ms_t": 0},
        tags=("params",),
    ),
    case(
        "016-no-word-separator-yet",
        "A single word with no separator is never eligible",
        "utf8::retract_to_word_boundary returns 0 when nothing below the offset ends a word. "
        "The first word of an utterance therefore commits only once a second word has begun, "
        "which is deliberate: a word ending is the most commonly revised thing there is.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": "hello"},
            {"op": "observe", "text": "hello"},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
        ],
        tags=("step4",),
    ),
    case(
        "017-empty-hypothesis",
        "An empty hypothesis is legal and publishes nothing",
        "A VAD-gated window that the model returned nothing for. Not an error: the utterance "
        "simply has no text yet.",
        [
            {"op": "reset", "utterance_id": 1},
            {"op": "observe", "text": ""},
            {"op": "observe", "text": ""},
            {"op": "advance", "ms": 300},
            {"op": "poll"},
            {"op": "finalize", "text": ""},
        ],
        tags=("edge",),
    ),
]


def run(case_dict):
    """Execute one case against the Python tracker, recording what it answered."""
    clock = ManualClock(0)
    params = StabilityParams(**case_dict["params"])
    tracker = StabilityTracker(params, clock=clock)
    out_steps = []

    for step in case_dict["steps"]:
        op = step["op"]
        recorded = dict(step)
        if op == "reset":
            tracker.reset(utterance_id=step.get("utterance_id", 0))
        elif op == "advance":
            clock.advance(int(step["ms"]))
        elif op in ("observe", "poll", "finalize"):
            try:
                if op == "observe":
                    value = tracker.observe(step["text"])
                elif op == "poll":
                    value = tracker.poll()
                else:
                    value = tracker.finalise(step["text"])
            except StabilityViolation as v:
                recorded["expect_violation"] = v.kind
                recorded["expect_diverge_offset"] = v.diverge_offset
                out_steps.append(recorded)
                # A violation ends the utterance: §8.2 binds the next message against the
                # previously published mark, so no value of stable_bytes makes a successor
                # conforming. The rest of the program is not executed, and the fixture says so
                # rather than leaving a reader to infer it from a truncated step list.
                out_steps.append({"op": "stop_after_violation"})
                break
            recorded["expect_stable_bytes"] = value
            recorded["expect_high_water"] = tracker.high_water
        else:
            raise SystemExit(f"unknown op {op!r} in case {case_dict['id']}")
        out_steps.append(recorded)

    result = dict(case_dict)
    result["steps"] = out_steps
    return result


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    cases = [run(c) for c in CASES]

    index = {
        "schema": 1,
        "description": (
            "Recorded behaviour of kvit_asr.stability.StabilityTracker, the oracle for "
            "src/asrd/stability.cpp. Regenerate with server/tools/gen_stability_fixtures.py; "
            "never edit an expect value by hand. openai-translator.md section 5 makes "
            "byte-for-byte agreement between the two trackers the T2 gate."
        ),
        "generated_by": "server/tools/gen_stability_fixtures.py",
        "step_grammar": {
            "reset": "{op, utterance_id} - clear the tracker for a new utterance",
            "advance": "{op, ms} - move the manual clock forward",
            "observe": "{op, text, expect_stable_bytes|expect_violation} - one hypothesis",
            "poll": "{op, expect_stable_bytes|expect_violation} - re-evaluate the dwell",
            "finalize": "{op, text, expect_stable_bytes|expect_violation} - close the utterance",
            "stop_after_violation": "{op} - the program ends here; a refused utterance has no "
                                    "conforming successor",
        },
        "cases": [{"id": c["id"], "file": f"{c['id']}.json", "name": c["name"],
                   "tags": c["tags"]} for c in cases],
    }

    (OUT / "index.json").write_text(
        json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for c in cases:
        (OUT / f"{c['id']}.json").write_text(
            json.dumps(c, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"wrote {len(cases)} cases + index.json to {OUT}")
    for c in cases:
        marks = [s.get("expect_stable_bytes", s.get("expect_violation"))
                 for s in c["steps"] if s["op"] in ("observe", "poll", "finalize")]
        print(f"  {c['id']}: {marks}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
