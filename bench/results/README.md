# `bench/results/` — result schema and how to read a cell

Written by `bench/run_matrix.py`. This directory is the experimental record for plan.md §4: one
JSON file per experimental cell, plus committed aggregates and an append-only audit trail of
every access to the held-out set.

The organising idea is that **a result must never be ambiguous about what produced it**. Every
file carries the full configuration, the split it used, the replay policy, the code version, and
every coverage cap that applied. If you are holding a number from this directory and cannot say
which model, which split, which chunk size and which harness version produced it, that is a bug
in the schema, not in your memory.

---

## 1. What is committed and what is not

plan.md §1.9: *"Benchmark artifacts under `bench/results/` may contain corpus text; the corpus
itself and any result file containing verbatim transcripts are gitignored, with only aggregate
metrics committed."*

The repository `.gitignore` implements that as deny-by-default with named exceptions:

| Path | Committed? | Why |
|---|---|---|
| `cells/<cell_key>.json` | **no** | Contains an `observations` block with hypotheses and references — verbatim corpus text. |
| `phase1.aggregate.json`, `phase2.aggregate.json` | **yes** | Metrics and configuration only. |
| `heldout-ledger.aggregate.json` | **yes** | Timestamps, purposes, configuration keys. No text. |
| `README.md` (this file) | yes | |

The rule is mechanical, not remembered: `run_matrix.assert_no_verbatim()` walks every aggregate
before it is written and refuses any structure containing a key from `TRANSCRIPT_KEYS` (`text`,
`hypotheses`, `reference`, `commit_trace`, `hallucination_texts`, `observations`, …). If you add
a field to an aggregate and the writer starts refusing it, the writer is right.

If you have to think about whether a file is safe to commit, it is not. Name it
`*.aggregate.json` only when it genuinely contains no transcript text.

---

## 2. Layout

```
bench/results/
├─ README.md                        this file
├─ cells/
│  └─ <cell_key>.json               one per experimental cell — GITIGNORED
├─ phase1.aggregate.json            Phase 1 rollup + the selection rule's output
├─ phase2.aggregate.json            Phase 2 rollup + the selection rule's output
└─ heldout-ledger.aggregate.json    append-only record of every heldout access
```

### The cell key

```
p2_nemotron-streaming-en-0-6b-onnx_tech_dev_ctxoff_7d2f0a91c4e5
│  │                                │    │   │      └ sha256 of the canonical config, 12 hex
│  │                                │    │   └ context mode: ctxoff | ctxon
│  │                                │    └ split: dev | heldout
│  │                                └ corpus category
│  └ model id, slugified
└ phase
```

The readable prefix is for humans running `ls`; **the digest is the identity**. Every field of
the configuration — including `replay_speed` and `repeats` — feeds the digest, so two cells that
differ in any respect cannot share a file. That is what makes the runner safely resumable: a
re-run skips a cell only when the cell it would run is byte-for-byte the same experiment.

There is a second key, `configuration_key`, which is the cell key with the corpus category
removed. The selection rule scores *configurations*, and a configuration is measured across
several categories (technical-term F1 from `tech`, hallucination from `silence`/`noise`), so the
aggregation step groups on this.

---

## 3. Cell file schema (`cells/<cell_key>.json`)

```jsonc
{
  "schema_version": 1,
  "cell_key": "p2_…",
  "configuration_key": "p2_…",      // cell key minus the corpus category
  "config": {                        // the full experimental condition
    "phase": 2,
    "model": "nemotron-streaming-en-0.6b-onnx",
    "category": "tech",              // tech | hard | clean | silence | noise
    "split": "dev",                  // dev | heldout
    "context_mode": "context_free",  // context_free | context_enabled — never pooled
    "chunk_ms_requested": 640,       // what plan.md §4's grid asked for
    "chunk_ms_actual": 560,          // what the model actually publishes — see §5
    "stability_n": 2,                // plan.md §1.4 step 1
    "stability_m": 3,                // plan.md §1.4 step 3
    "stability_t_ms": 200,           // plan.md §1.4 step 5
    "cadence": "immediate",          // flush timing only — NEVER eligibility
    "inject_method": "SendInput",
    "replay_speed": 1.0,             // 1.0 = real time
    "repeats": 3
  },
  "config_digest": "7d2f0a91c4e5…",  // sha256 of the above, canonical JSON
  "split": "dev",                    // duplicated deliberately: the split is the discipline
  "purpose": "tuning",
  "status": "ok",                    // ok | failed
  "error": null,
  "synthetic": false,                // true = MockExecutor; NOT a measurement
  "executor": "sidecar",
  "replay": {
    "speed": 1.0,
    "realtime": true,
    "frame_ms": 20,
    "warning": null                  // non-null iff the run was NOT real time
  },
  "code_version": {
    "git_head": "…", "git_dirty": true,
    "run_matrix_sha256_16": "…", "metrics_sha256_16": "…",
    "metrics_versions": {            // from metrics.metric_versions()
      "metrics": "metrics-1", "normaliser": "norm-1",
      "terms": "terms-1", "grapheme": "utf8-approx-1/…"
    },
    "python": "3.12.x", "platform": "win32"
  },
  "utc": "2026-08-16T12:34:56Z",
  "clips": { "n": 48, "ids": ["tech-0001.16k.wav", …] },
  "metrics": { … },                  // see §4
  "per_repeat": [ { … }, { … }, { … } ],
  "coverage_caps": [ { "kind": "chunk_substituted", "scope": "…", "detail": "…" } ],
  "observations": { … }              // verbatim text lives here; never in an aggregate
}
```

### Reading one

1. **`status` and `synthetic` first.** A `failed` cell has no metrics, and its configuration will
   be eliminated by the selection rule as `missing_metric:*` — *not* as a loser. A `synthetic`
   cell was produced by `MockExecutor` and measures nothing at all.
2. **`replay.warning`.** If it is non-null, the latency numbers in this file were produced by
   feeding audio faster than real time and are not latency measurements.
3. **`coverage_caps`.** Every place this cell covered less than the nominal grid.
4. **`config.chunk_ms_requested` vs `chunk_ms_actual`.** If they differ, the number is for the
   *actual* chunk size.
5. **`per_repeat`.** `metrics` holds the median plus `__min` / `__max` / `__stdev` / `__n`
   suffixed spreads. plan.md §4 asks for run-to-run variance, so a single sample is not a result;
   median rather than mean because one Windows scheduling hiccup produces an enormous outlier and
   a mean would carry it into the decision.

---

## 4. Metric keys

Metric computation lives in `bench/metrics.py`; the runner consumes the following keys. Five of
them gate the selection rule and are therefore mandatory — a configuration missing any one is
eliminated with a stated reason rather than dropped.

The two modules do not share a single spelling for every metric — `metrics.py` calls the
objective `term_f1`, the runner calls it `technical_term_f1` — so `run_matrix.METRIC_ALIASES`
holds one canonical name per quantity plus the spellings accepted on input, and neither module
renames the other's metric. `run_matrix.check_gate_metrics_agree()` compares that table against
`metrics.select_gate_metrics()` on every run and logs a `metric_unavailable` coverage cap if
`metrics.py` nominates a gate the runner cannot read — the failure being defended against is a
gate that silently stops being applied.

Ruleset versions (`normaliser`, `terms`, `grapheme`) are copied from `metrics.metric_versions()`
into `code_version.metrics_versions` in every cell file. A number produced under a different
normaliser or term table is not comparable to one produced under this one, and recording the
versions is what makes that mismatch visible instead of invisible.

| Key | Gates? | Meaning |
|---|---|---|
| `technical_term_f1` (`term_f1`) | **objective** | plan.md §4's maximand. Taken from the `tech` category only — `clean` (LibriSpeech) has no technical vocabulary and averaging it in would dilute exactly the quantity being maximised. |
| `technical_term_f1_ruleset` | no | Version of the matching ruleset, so a later rule change cannot silently rewrite history. |
| `icer_per_1000` | **gate** | Irrevocable committed error rate: committed graphemes differing from the final hypothesis, per 1000 committed (plan.md §1.4). The headline safety number. |
| `icer_error_graphemes`, `icer_committed_graphemes` | no | The numerator and denominator. When both are present the aggregate pools ICER by its true denominator; when they are absent it falls back to the mean of per-cell rates and **records that fallback as a coverage cap**, because a mean of rates is not a rate. |
| `first_partial_ms` | **gate** | plan.md Phase 2 exit target: < 300 ms. |
| `finalisation_ms` | **gate** | plan.md Phase 2 exit target: < 500 ms after speech end. |
| `peak_vram_mb` | **gate** | Aggregated with `max`, not mean: a configuration that fits on four categories and not the fifth does not fit. |
| `wer_raw`, `wer_norm`, `cer_raw`, `cer_norm` | no | plan.md §4 requires both. **Raw is the one that matters** — normalisation hides `llama.cpp` vs `llama dot cpp`, which is the error class this product exists to fix. |
| `flicker_per_1000` | no | Churn the user never sees. Recorded, deliberately not a gate: flicker can be high with zero document corruption. ICER gates; flicker does not. |
| `latency_median_ms` | tie-break | `metrics.select_gate_metrics()` nominates this as the second tie-break, after ICER. When absent the tie falls through to `first_partial_ms` then `finalisation_ms`. |
| `rtf`, `cold_start_ms`, `warm_ms` | no | |
| `hallucination_rate` | opt-in | From the `silence`/`noise` cells. **Not part of the pre-registered rule.** It can be turned into a gate with `--limit-hallucination-rate`, and any elimination it causes is labelled `extra_gate` so the pre-registered rule stays legible underneath. |
| `latency_decomposition` | no | Algorithmic / queueing / inference / endpoint / UI-commit, measured separately (plan.md Phase 2 step 4). |

---

## 5. Coverage caps

plan.md §4: *"Any coverage cap — top-N, sampling, no-retry — is logged; silent truncation reads
as 'we tested everything'."* Caps appear in the cell file, in the phase aggregate, and in a
block of the run summary printed on stdout.

| `kind` | Raised when |
|---|---|
| `corpus_sampling` | `--max-clips` dropped clips. Records the fraction. |
| `chunk_substituted` | The requested chunk size is not published for this model; the nearest was used. See below. |
| `chunk_unsupported` | The model has no chunk-size knob at all (the sherpa-onnx zipformer bakes it into the export). |
| `cell_dropped` | A cell was excluded before running — most often a context-enabled cell for a backend that reports `supports_context = false`. |
| `cell_failed` / `no_retry` | The cell ran and failed, and (by default) was not retried. Its configuration is eliminated as `missing_metric`, not as a loser. |
| `max_cells` | `--max-cells` truncated the grid. Records how many cells did not run. |
| `top_n` | Only the top N survivors were carried into the next phase. |
| `models_excluded` | A model was excluded from a phase — e.g. the second-tier batch references are not in the Phase 2 streaming sweep (CANDIDATES.md §4). |
| `categories_excluded` | A corpus category was not run. If `silence`/`noise` are among them, the cap says outright that the hallucination check did not happen. |
| `repeats_reduced` | Fewer than 3 repeats, so the reported spread means little. |
| `metric_unavailable` | A metric could not be computed, or was pooled by a documented approximation. |
| `grapheme_approximation` | Grapheme counting fell back to code points instead of the client's UAX#29 approximation (`src/util/utf8.h`), which inflates ICER on emoji and combining sequences. |

**On `chunk_substituted`, because it is the least obvious one.** plan.md §4 asks for
`chunk_ms ∈ {80, 160, 320, 640}`. Per `bench/CANDIDATES.md` §2, the sherpa-onnx Nemotron exports
are *one Hugging Face repo per chunk size* — `{80, 160, 560, 1120}` for the English model — so
chunk size selects a different artifact rather than setting a runtime parameter. A request for
640 ms runs the 560 ms artifact. Both numbers are in the cell file and the substitution is a
logged cap, because a table with a "640 ms" column that silently holds 560 ms measurements is
worse than no table.

---

## 6. The phase aggregate

`phaseN.aggregate.json` holds, for one phase and split:

- `thresholds` — the gates *and their provenance*. `_sources` says, per threshold, whether
  plan.md states the number or whether this project declared it. Two of them are ours: plan.md
  §4 gates on "ICER below its declared threshold" without declaring one, and states no VRAM
  figure at all. They are declared in code, before the corpus runs, rather than chosen at report
  time.
- `configurations` — one entry per `configuration_key` with the aggregated metrics and any
  `missing_metrics`.
- `selection` — the pre-registered rule's output, **once per context mode**, each with `winner`,
  `ranked`, `eliminated` (with reasons) and `notes`.
- `coverage_caps` — the run's caps.
- `any_synthetic` — true if any input was a mock result.

### How the selection rule reads

plan.md §4, quoted, then implemented in `run_matrix.apply_selection_rule()`:

> maximise held-out technical-term F1 among configurations that meet the first-partial,
> finalisation, ICER, and VRAM limits. Ties break on ICER, then latency.

Four behaviours worth knowing before you read an output:

1. **Every non-winner carries a reason.** A configuration that was never measured is eliminated
   as `missing_metric:<key>` — it must not be indistinguishable from one that lost.
2. **The runner-up case is announced.** If the highest-F1 configuration fails a gate, the notes
   say so by name, with its F1 and the gate that killed it, and state that the winner is the
   runner-up. A rule that shows only its winner is not auditable.
3. **Context modes are never pooled.** The rule runs once for `context_free` and once for
   `context_enabled`. CANDIDATES.md §5.1 is the concrete reason: the leading streaming candidate
   is `greedy_search`-only and cannot be context-biased at all, while the zipformer arm can.
   Pooling them would compare a biased model against an unbiased one on the metric that biasing
   helps most.
4. **`dev` selections are labelled `NOT A DECISION`, and synthetic results are refused.** The
   rule decides on the held-out set, from measurements. `--allow-synthetic` exists only to
   exercise the rule itself.

---

## 7. The held-out ledger

`heldout-ledger.aggregate.json` is append-only. Every `run --split heldout` writes an entry with
a timestamp, the stated `--purpose`, the configuration keys touched, and the code version.

plan.md Phase 1 step 2: the held-out set is *"the decision set. Touched once, at selection time.
Never tuned on."* The runner refuses `--split heldout` without `--heldout-decision`, and the
ledger makes the "touched once" part checkable rather than merely asserted. More than one entry
is not automatically wrong — a re-run after a harness bug is legitimate — but it is something a
reader of the results is entitled to see without asking.

A corrupt ledger is an error, not something to overwrite.

---

## 8. Typical commands

```bash
# See the grid without running anything.
python bench/run_matrix.py plan --phase 1

# Phase 1: accuracy cut on dev/, all four CANDIDATES.md §4 picks, real-time replay.
python bench/run_matrix.py run --phase 1 --split dev --executor sidecar

# Phase 2: sweep the streaming knobs over the Phase 1 survivors only.
python bench/run_matrix.py run --phase 2 --split dev --executor sidecar \
    --models nemotron-streaming-en-0.6b-onnx,streaming-zipformer-en-2023-06-26 \
    --phase2-full-sweep

# The decision run. Refused without the flag; recorded in the ledger.
python bench/run_matrix.py run --phase 2 --split heldout --heldout-decision \
    --purpose "phase 2 selection" --executor sidecar

# Re-apply the rule to results that already exist.
python bench/run_matrix.py select --phase 2 --split heldout
```

Re-running is safe: a cell with an `ok` result is skipped unless `--force`. A `failed` cell is
retried on the next run, so a transient failure does not permanently remove a configuration from
the table.

`--speed` above 1.0 requires `--allow-faster-than-realtime` and stamps the results NOT REAL TIME.
