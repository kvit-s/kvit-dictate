# bench/ — the offline bake-off harness

Everything that decides *which model ships* lives here. plan.md §4 states the experiment design
and the selection rule before the corpus is run, and this directory is that document turned
into code and data: the candidate survey, the prompt sets, the recorder, the metrics, the
runner, and the results.

The one-line summary of the design: **the rule is written down first, the split is enforced by
machinery, and every coverage cap is logged**, because the failure this harness exists to
prevent is not a wrong number — it is a right-looking number chosen after the fact.

---

## 1. Index

| Path | Owner of | Status |
|---|---|---|
| `CANDIDATES.md` | Phase 0A: the model survey, the shortlist, and the findings that changed the plan. | **Done** (2026-08-16) |
| `prompts/tech.jsonl` | 55 technical sentences — identifiers, paths, CLI flags, model ids, versions. Each carries its `split` and its expected technical terms. | Written |
| `prompts/hard.jsonl` | 28 hard-delivery sentences — self-correction, fast speech, trailing-off ends, mid-sentence pauses, noise, disfluency, homophones. Each carries a `delivery` instruction to the operator. | Written |
| `prompts/ambient.jsonl` | 3 silence-only + 5 noise-only clips with **empty references**, for hallucination testing. | Written |
| `record.py` | The corpus recorder. Prompting, file naming, metadata, the dev/held-out split, the ledger, the Win+H comparison, and `verify`. Drives the C++ capture path; opens no microphone itself. | Written; **blocked on `kd_record.exe`** (§4) |
| `corpus/README.md` | The corpus: categories, split discipline, layout, sidecar schema, ledger. | Written |
| `metrics.py` | WER/CER raw **and** normalised, technical-term F1 over a pre-registered alias table, ICER, flicker, latency decomposition, RTF, VRAM, hallucination detection. | Written |
| `run_matrix.py` | The runner: sequential elimination, one JSON per cell, resumable, real-time replay, mechanical held-out guard, the pre-registered selection rule as code. | Written |
| `results/README.md` | Result file schema and what may be committed. | See that file |
| `tests/` | `python -m unittest discover -s bench/tests -t .` | Written |

Stdlib only, CPython 3.12. Nothing in `bench/` imports a third-party package; that is a
deliberate property, not an accident, and it is what lets the harness run on a machine whose
model environments are broken.

---

## 2. What gets committed

From plan.md §1.9 and the rules already in `.gitignore`:

- **Committed:** this file, `CANDIDATES.md`, `prompts/*.jsonl`, all the Python, and aggregate
  result files named `*.aggregate.json` / `*.aggregate.csv`.
- **Gitignored:** `corpus/` in its entirety (audio and transcripts), and every per-cell result
  file under `results/`, because those contain verbatim hypotheses.

The prompt lists are committed on purpose: the sentences are the design and are meant to be
reviewed and argued with, while the audio is one person's rendering of them and is private.

> `.gitignore` currently ignores `bench/corpus/` wholesale, which also hides
> `bench/corpus/README.md`. Adding `!bench/corpus/README.md` after that rule would commit the
> documentation without the audio. Until that happens, the split rules are duplicated in §3
> below so they live somewhere that *is* in version control.

---

## 3. The dev / held-out split, in one place

`heldout/` is the **decision set**: recorded once, evaluated once, at selection time, never
tuned on. `dev/` is everything else — vocabulary, prompts, normalisation, VAD, stability
parameters.

Enforced at two points, by machinery rather than by discipline:

**At recording time (`record.py`).** Each prompt's split is committed data, so the destination
directory is not a runtime choice; recording into `heldout/` needs `--split heldout`,
`--yes-heldout`, and a typed confirmation phrase; accepted held-out files are made read-only;
there is no command that moves a clip between splits; and `record.py verify` cross-checks the
directory, the sidecar and the committed prompt file against each other plus a hash-chained
ledger.

**At run time (`run_matrix.py`).** A run against `heldout/` is refused without
`--heldout-decision`, every held-out access is appended to
`results/heldout-ledger.aggregate.json`, and every result file records which split produced it.

---

## 4. Order of operations

Phase 1 (plan.md §3) runs in this order, and the order is the point:

**1. Build the real capture path first.** `src/audio_capture.{h,cpp}` — done. plan.md §9:
revision 1 planned to record the corpus with a Python recorder while scheduling WASAPI capture
for Phase 3, which would have tuned models against a microphone pipeline the product does not
use.

**2. `kd_record.exe` — NOT YET BUILT.** `record.py` drives a small console tool over
`kd::audio::AudioCapture` + `kd::audio::WavWriter`; `tools/probe/main.cpp` is still a Phase 0B
placeholder and no recording entry point exists. The stdin/stdout contract `record.py` is
written against is specified in full in `record.py`'s module docstring (`--list-devices`
prints one JSON object; `--record` prints `READY`, records until a line on stdin or
`--max-seconds`, writes both WAVs and a metadata JSON, prints `DONE`). The contract version is
checked on every invocation and a mismatch is a hard refusal.

**3. Record the corpus.** `record.py record --category tech`, then `hard`, then
`silence`/`noise`. Then, separately and once, the held-out half.

**4. Record the Win+H comparison.** `record.py winh`. plan.md §4 requires it as a sanity check:
shipping something worse than the built-in would be the real failure.

**5. Run the bake-off.** `run_matrix.py plan` to see the grid, `run` to execute it against
`dev/`, `select` to apply the pre-registered rule. The held-out run happens once, at the end,
with `--heldout-decision`.

**6. `record.py verify` before every bake-off run.** It is cheap and it is the only thing that
notices a corpus that has been reorganised by hand.

---

## 5. Quick reference

```powershell
py -3.12 bench\record.py --help
py -3.12 bench\record.py devices
py -3.12 bench\record.py status
py -3.12 bench\record.py record --category tech --operator sk --notes "AT2020, 20 cm"
py -3.12 bench\record.py verify

py -3.12 bench\run_matrix.py plan --phase 1
py -3.12 bench\run_matrix.py run  --phase 1 --split dev
py -3.12 bench\run_matrix.py select --phase 1

C:\Python312\python.exe -m unittest discover -s bench\tests -t .
```

`record.py --simulate` exercises the recorder's bookkeeping without a microphone or a C++
build. It writes synthetic tones into `corpus/_simulated/`, keeps its own ledger, and `verify`
treats simulated audio inside the real corpus as a hard failure. It is not a recording mode.

---

## 6. What the numbers mean, briefly

The full definitions are in `metrics.py`; the two things worth repeating here because they are
what a reader is most likely to get backwards:

- **Raw WER is the number that matters**, not normalised WER. `llama.cpp` transcribed as
  "llama dot cpp" is exactly the error class this product exists to fix, and a normaliser that
  rescues it is broken for our purpose. Both are always reported side by side.
- **ICER gates; flicker does not.** Flicker is churn the user never sees and can be high with
  zero document corruption. ICER — irrevocable committed error rate, committed graphemes that
  differ from the final hypothesis, per 1000 committed — measures text that is actually wrong
  in the actual document. plan.md §1.4 makes it the headline safety number.

And one thing that is a property of the corpus rather than of the metrics: the technical-term
F1 that the selection rule maximises is computed over `tech/` only. `clean/` is a LibriSpeech
sanity slice with no technical vocabulary in it, and pooling it in would dilute the number
toward meaninglessness.
