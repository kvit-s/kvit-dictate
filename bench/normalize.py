#!/usr/bin/env python3
"""bench/normalize.py — the do-no-harm test for the text normalizer.

Implements the measurement `src/normalizer.h`'s term guard was written against. Stdlib only,
CPython 3.12, in keeping with the rest of bench/.

────────────────────────────────────────────────────────────────────────────────────────────
WHAT THIS MEASURES, AND WHY IT NEEDS NO AUDIO AND NO ASR
────────────────────────────────────────────────────────────────────────────────────────────

`superwhisper/s1-mini` is a rewriter. The interesting question is not "does it clean up
speech" — the model card answers that — but **"what does it damage"**, and that question can
be asked without a microphone, without a recogniser and without a corpus.

The prompts in `bench/prompts/*.jsonl` are already the ground truth of what the speaker meant.
Feed a *correct* transcript to a normalizer and a correct normalizer changes almost nothing: it
is already written text. So sending the reference text itself is a pure damage test, and it is
the test that decides whether this model is admissible to a program whose whole premise is
getting `sm_120`, `--n-gpu-layers` and `C:\\uv-cache` exactly right.

`bench/metrics.py:675` predicted the failure before this model existed — `"sm 120"` is listed
as a known-bad rendering of `sm_120`. This script is what turns that prediction into a number.

**Losing a term from the built-in table is a veto**, as is any `wrong_form` hit. Not a score to
be traded off: the app's guard (`src/normalizer.h`) rejects a rewrite that loses a protected
term, so a model that trips this often is a model whose cleanups mostly never reach the user —
which is worse than leaving the feature off.

The per-prompt `terms` in the corpus are a *different* population and are never vetoed. Several
of them are meant to disappear: `tech-033`'s "46 percent" becoming "46%" is the inverse text
normalisation the model exists to do, and `hard-001`'s "30" is the false start it is supposed to
resolve away. They are reported as "authored term gone (may be correct)" and left to a human.

────────────────────────────────────────────────────────────────────────────────────────────
WHAT THIS DOES **NOT** MEASURE
────────────────────────────────────────────────────────────────────────────────────────────

Whether the normalizer is any *good*. That needs paired data this repository does not have:
the raw ASR transcript on one side and the intended written text on the other. The `hard`
prompts make the gap explicit — `hard-001` says "Reference is verbatim as spoken, including the
correction" and `hard-002` says "Do not clean it up" — so against those references a *correct*
normalisation scores as pure WER error. That is why `--split hard` is reported separately and
its WER is labelled as uninterpretable rather than being averaged into a headline.

Recording that paired corpus is Phase 1 work. This script is the gate that comes first.

────────────────────────────────────────────────────────────────────────────────────────────
USAGE
────────────────────────────────────────────────────────────────────────────────────────────

    python bench/normalize.py --url http://strixhalo.lan:10301/v1/text/normalize
    python bench/normalize.py --url ... --category tech        # just the technical prompts
    python bench/normalize.py --url ... --json results.json    # machine-readable, for a ledger
    python bench/normalize.py --url ... --quant Q4_K_M         # recorded in the output header

`--quant` is a **label, not a request**. Nothing about it is sent to the server: the endpoint
serves whatever weights it was started with and has no dtype parameter, so this flag exists only
so the JSON records which build the operator believes they measured. Getting it wrong mislabels
a result; it cannot change one.

Record it anyway, because the model card's 94.8% token accuracy was measured on `Q4_K_M` and a
deployment serving bf16 is a different artifact. Note the direction: bf16 is the *unquantised*
model, so it is the ceiling of what these weights do. Damage measured there cannot be blamed on
quantisation.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import metrics  # noqa: E402  (path set above, as elsewhere in bench/)

PROMPTS_DIR = pathlib.Path(__file__).resolve().parent / "prompts"

# Mirrors src/normalizer.cpp's `guard_verdict` length floor. Kept as a module constant with the
# C++ file named, because the point of this script is to decide whether that number is right; a
# bench that silently used a different one would be measuring a program nobody ships.
GUARD_MIN_RATIO = 0.5


@dataclass
class Case:
    id: str
    category: str
    split: str
    text: str
    terms: list[str]
    notes: str = ""


@dataclass
class Outcome:
    case: Case
    normalized: str = ""
    elapsed_ms: float = 0.0
    error: str = ""
    # Populated only on a successful call.
    f1: float = 0.0
    wrong_forms: list[tuple[str, str]] = field(default_factory=list)
    lost_terms: list[str] = field(default_factory=list)
    lost_protected: list[str] = field(default_factory=list)
    lost_authored: list[str] = field(default_factory=list)
    wer_raw: float | None = None
    guard_rejected: str = ""

    @property
    def ok(self) -> bool:
        return not self.error


def load_cases(categories: set[str] | None) -> list[Case]:
    cases: list[Case] = []
    for path in sorted(PROMPTS_DIR.glob("*.jsonl")):
        for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"{path.name}:{line_no}: {exc}") from exc
            if categories and d.get("category") not in categories:
                continue
            # bench/prompts/ambient.jsonl carries recording prompts, not transcripts: `text`
            # is empty because the point is what the microphone hears in a silent or noisy room.
            # There is nothing for a text normalizer to damage, and counting "" -> "" as a
            # rejection would put eight meaningless failures in every summary.
            if not d["text"].strip():
                continue
            cases.append(Case(id=d["id"], category=d.get("category", "?"),
                              split=d.get("split", "?"), text=d["text"],
                              terms=list(d.get("terms", ())), notes=d.get("notes", "")))
    return cases


def normalize_once(url: str, text: str, *, styling: str, structure: str, context: str,
                   timeout: float) -> tuple[str, float]:
    """POST one transcript. Returns (normalized_text, elapsed_ms). Raises on failure."""
    body = json.dumps({
        "text": text,
        "styling": styling,
        "structure": structure,
        "context": context,
        "response_format": "json",
    }).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json"})
    began = time.perf_counter()
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (operator-supplied)
        payload = json.loads(resp.read().decode("utf-8"))
    elapsed = (time.perf_counter() - began) * 1000.0
    if "text" not in payload:
        raise ValueError(f"response has no 'text' member: {sorted(payload)}")
    return payload["text"], elapsed


def guard_verdict(raw: str, normalized: str, terms: list[str]) -> str:
    """The app's guard, in Python, so the bench scores what the program will actually do.

    Deliberately a re-implementation rather than a binding: it is short, and the point of having
    it on both sides is that a disagreement between them shows up as a bench result that does not
    match the field, which is exactly the kind of drift a shared binding would hide.
    """
    if not normalized.strip():
        return "RejectEmpty"
    if raw and len(normalized) / len(raw) < GUARD_MIN_RATIO:
        return "RejectTooShort"
    raw_l, norm_l = raw.lower(), normalized.lower()
    for term in terms:
        if not term:
            continue
        t = term.lower()
        if t in raw_l and t not in norm_l:
            return f"RejectTermLost:{term}"
    return "Accept"


def run(args: argparse.Namespace) -> int:
    categories = set(args.category) if args.category else None
    cases = load_cases(categories)
    if not cases:
        print("no prompts matched", file=sys.stderr)
        return 2

    matcher = metrics.TermMatcher()
    builtin = [t.canonical for t in metrics.TERMS]

    print(f"# normalize do-no-harm — {len(cases)} prompts")
    print(f"# endpoint : {args.url}")
    print(f"# weights  : {args.quant or '(unlabelled — pass --quant, it is a label only)'}")
    print(f"# controls : styling={args.styling} structure={args.structure} "
          f"context={args.context}")
    print()

    outcomes: list[Outcome] = []
    for case in cases:
        out = Outcome(case=case)
        try:
            out.normalized, out.elapsed_ms = normalize_once(
                args.url, case.text, styling=args.styling, structure=args.structure,
                context=args.context, timeout=args.timeout)
        except (urllib.error.URLError, ValueError, TimeoutError, OSError) as exc:
            out.error = f"{type(exc).__name__}: {exc}"
            outcomes.append(out)
            print(f"  {case.id:<10} ERROR  {out.error}")
            continue

        report = metrics.term_f1(case.text, out.normalized, matcher=matcher)
        out.f1 = report.f1
        out.wrong_forms = list(matcher.find_wrong_forms(out.normalized))
        # The per-prompt term list is the authored one; the built-in table is what the app
        # enforces. Both are checked, because a prompt may name a term the table does not know.
        # Two different populations, and conflating them makes the result unreadable.
        #
        #   `lost_protected` — the built-in table, which is what src/normalizer.cpp's guard
        #   enforces. A loss here is damage and vetoes the run.
        #
        #   `lost_authored`  — the per-prompt `terms` the corpus author listed for scoring. Some
        #   of these are *meant* to disappear: "46 percent" becoming "46%" is the ITN the model
        #   exists to do, and hard-001's "30" is the false start it is supposed to resolve away.
        #   Reported, never vetoed.
        lost = lambda ts: sorted({t for t in ts if t and t.lower() in case.text.lower()
                                  and t.lower() not in out.normalized.lower()})
        out.lost_protected = lost(builtin)
        out.lost_authored = [t for t in lost(case.terms) if t not in out.lost_protected]
        out.lost_terms = out.lost_protected + out.lost_authored
        rates = metrics.error_rates(case.text, out.normalized)
        out.wer_raw = rates.wer_raw.rate
        # The app's guard sees the built-in table plus the user's own context terms — never the
        # bench's authored per-prompt list, which does not exist at run time.
        out.guard_rejected = guard_verdict(case.text, out.normalized, builtin)

        flag = "!!" if (out.lost_protected or out.wrong_forms) else "  "
        print(f"{flag}{case.id:<10} f1={out.f1:5.3f}  wer={out.wer_raw or 0.0:5.3f}  "
              f"{out.elapsed_ms:6.0f}ms  {out.guard_rejected}")
        for canonical, form in out.wrong_forms:
            print(f"      WRONG FORM  {canonical!r} rendered as {form!r}")
        if out.lost_protected:
            print(f"      DAMAGED     {out.lost_protected}")
            print(f"      became      {out.normalized!r}")
        if out.lost_authored:
            print(f"      authored term gone (may be correct): {out.lost_authored}")
        if args.verbose:
            print(f"      in   {case.text!r}")
            print(f"      out  {out.normalized!r}")
        outcomes.append(out)

    return report_summary(outcomes, args)


def report_summary(outcomes: list[Outcome], args: argparse.Namespace) -> int:
    ok = [o for o in outcomes if o.ok]
    failed = [o for o in outcomes if not o.ok]
    wrong_form_hits = [o for o in ok if o.wrong_forms]
    damaged = [o for o in ok if o.lost_protected]
    rejected = [o for o in ok if o.guard_rejected != "Accept"]

    print()
    print("─" * 92)
    if failed:
        print(f"  {len(failed)} of {len(outcomes)} calls FAILED — the numbers below cover the rest")
    if ok:
        lat = sorted(o.elapsed_ms for o in ok)
        mean_f1 = sum(o.f1 for o in ok) / len(ok)
        print(f"  term F1 (mean)      {mean_f1:.4f}   over {len(ok)} prompts")
        print(f"  latency  p50/p95    {lat[len(lat) // 2]:.0f} / "
              f"{lat[min(len(lat) - 1, int(len(lat) * 0.95))]:.0f} ms")
        print(f"  guard rejections    {len(rejected)} ({100.0 * len(rejected) / len(ok):.1f}%)")
        for o in rejected:
            print(f"      {o.case.id:<10} {o.guard_rejected}")

    # The categories are not comparable and must not be averaged into one headline.
    for cat in sorted({o.case.category for o in ok}):
        sub = [o for o in ok if o.case.category == cat]
        wer = sum(o.wer_raw or 0.0 for o in sub) / len(sub)
        note = ""
        if cat == "hard":
            # bench/prompts/hard.jsonl's references are verbatim-as-spoken, including the
            # self-correction ("Do not clean it up"). A correct normalisation is an error against
            # them by construction, so this number says nothing about quality either way.
            note = "  (uninterpretable: references are verbatim-as-spoken)"
        print(f"  raw WER {cat:<8}    {wer:.4f}{note}")

    print()
    if damaged or wrong_form_hits:
        n = len({id(o) for o in damaged} | {id(o) for o in wrong_form_hits})
        print(f"  VETO — {n} prompt(s) damaged a term this program exists to get right.")
        for o in damaged:
            print(f"      {o.case.id:<10} {o.lost_protected}")
            print(f"                 {o.case.text!r}")
            print(f"              -> {o.normalized!r}")
        print()
        print("  The guard in src/normalizer.cpp rejects every one of these, so the app is safe")
        print("  — it keeps the raw text. But a model that has to be overruled this often is a")
        print("  model whose cleanups mostly never reach the user, which is worse than off.")
        verdict = 1
    elif failed:
        print("  INCONCLUSIVE — some calls did not complete.")
        verdict = 2
    else:
        print("  PASS — no protected term was damaged in any prompt.")
        print("  This says the model does no harm to already-correct text. It says nothing")
        print("  about whether its cleanups are any good; that needs the paired corpus.")
        verdict = 0
    print("─" * 92)

    if args.json:
        payload = {
            "endpoint": args.url,
            "quant": args.quant,
            "controls": {"styling": args.styling, "structure": args.structure,
                         "context": args.context},
            "guard_min_ratio": GUARD_MIN_RATIO,
            "term_ruleset_version": metrics.TERM_RULESET_VERSION,
            "verdict": ["pass", "veto", "inconclusive"][verdict],
            "cases": [
                {"id": o.case.id, "category": o.case.category, "split": o.case.split,
                 "input": o.case.text, "output": o.normalized, "elapsed_ms": o.elapsed_ms,
                 "error": o.error, "term_f1": o.f1, "wer_raw": o.wer_raw,
                 "wrong_forms": [list(w) for w in o.wrong_forms],
                 "lost_protected": o.lost_protected, "lost_authored": o.lost_authored,
                 "guard": o.guard_rejected}
                for o in outcomes
            ],
        }
        pathlib.Path(args.json).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"  wrote {args.json}")

    return verdict


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True, help="the /v1/text/normalize endpoint")
    ap.add_argument("--category", action="append",
                    help="tech | hard | ambient (repeatable; default all)")
    ap.add_argument("--styling", default="semi-formal")
    ap.add_argument("--structure", default="prose")
    ap.add_argument("--context", default="general")
    ap.add_argument("--timeout", type=float, default=30.0, help="seconds per call")
    ap.add_argument("--quant", default="",
                    help="label for the weights being served, e.g. bf16 or Q4_K_M. Recorded in "
                         "the output only — the endpoint has no dtype parameter and nothing is "
                         "sent to it")
    ap.add_argument("--json", help="write a machine-readable result file")
    ap.add_argument("--verbose", action="store_true", help="print every input and output")
    return run(ap.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
