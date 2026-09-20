# `bench/results/ab/` — service A/B runs

Written by `bench/ab_service.py`. One directory per run. **Only `*.aggregate.json` is
committed**: every other file in a run directory carries verbatim text from real dictation, and
the harness drops a `.gitignore` into each run directory to enforce that, because the
repository's own `.gitignore` re-admits `bench/results/**/*.md` and would otherwise commit
`adjudicate.md`.

These runs are **not** the plan.md §4 bake-off. That is `run_matrix.py`, it decides which model
ships, it needs a corpus with written references, and it still has none. These runs answer a
narrower question against the corpus that does exist — `src/corpus.h`'s capture of real sessions
— and they cannot report accuracy, only divergence. See the module docstring of
`ab_service.py` for why that distinction is load-bearing.

---

## 2026-09-20-qwen-vs-r2t2

`Qwen/Qwen3-ASR-1.7B` at `strixhalo.lan:10301` (the incumbent — what `settings.json` points at)
against `netease-youdao/Confucius4-R2T2` at `192.168.138.186:10311`.

R2T2 is a streaming fine-tune **of Qwen3-ASR-1.7B**, trained for append-only output via a
Longest Stable Prefix objective. So this is not two architectures; it is one model against its
own streaming derivative, which is why so much of what follows is about *emission behaviour*
rather than recognition.

200 sessions sampled uniformly from 746, seed 7. 396 speech clips and 65 open-microphone gaps,
segmented by `kvit_asr.vad.VadEndpointer` at the operator's `endpoint_silence_ms=1500`. Both
services asked about every clip, alternating which went first.

**Reproduction guard: 97.2 %** of words agree between the incumbent replayed over the
re-segmented recording and what it committed live, across 197 sessions, 107 of them
word-identical. The segmentation is the one the live run used.

### What the run found

| | qwen | r2t2 |
|---|---|---|
| hallucinated over an open microphone | **65 / 65** | **3 / 65** |
| answers truncated mid-sentence | 0 | 11 (2.8 %) |
| empty answers on speech | 0 | 10 (2.5 %) |
| latency mean / p90 / p99 (ms) | 228 / 400 / 488 | 216 / 378 / 468 |
| terminal `.` `!` `?` present | 388 / 396 | 249 / 396 |
| first character lower-case | 0 / 396 | 43 / 396 |

Divergence between the two, which is symmetric and is not an error rate: 21.7 % of clips
identical verbatim, 51.5 % identical after normalisation, 12.2 % raw word divergence and 5.5 %
normalised (5.1 % once truncated clips are excluded).

Of the 396 clips, 43.2 % differ in words, 29.8 % differ only in case or punctuation, 21.7 % are
identical, 2.8 % are truncated and 2.5 % are empty on one side.

### The four findings, in the order they matter

1. **Hallucination over non-speech is the difference, and it is not close.** Handed real room
   tone the endpointer had already rejected, the incumbent invented words on every single clip —
   36 of them `"Okay."` — and R2T2 on three, two of which look like speech the gate wrongly
   dropped rather than invention. progress.md §4 records the incumbent's 5-byte commits the
   operator pressed Esc on; this is that defect, counted. It is also why
   `openai_http.py`'s `vad_gate=true` exists, and a model that does not need the gate is worth
   more than the gate.

2. **R2T2's truncation is a deployment bug, not a quality result.** It ends 2.8 % of answers
   mid-sentence with a literal `|`, its Longest Stable Prefix boundary, losing a mean of 2.3
   words and up to 7. The wrapper at `:10311` is running the streaming path and never calling
   `finish_streaming_transcribe`, so the unstable tail is never flushed. No form field on that
   endpoint changes it — `stream`, `offline`, `mode`, `finish`, `chunk_size_ms`,
   `unfixed_token_num` were all tried and all ignored. **Nothing about R2T2's accuracy should be
   concluded until this is fixed**, and the fix is on the server.

3. **Technical vocabulary goes to R2T2**, which is the thing this product exists for. It
   returned file names with their real extensions, key combinations in `Ctrl+X` form, and
   product names in their real casing, in cases where the incumbent returned a plausible English
   phrase instead. This is a read of the disagreement set, not a measured term-F1 — there is no
   reference to score against — so it is stated as a direction, not a number.

4. **R2T2's casing and punctuation are worse raw and mostly repaired downstream.** 152 of 396
   answers start lower-case or lack terminal punctuation. The operator runs the normalizer
   (`docs/normalizer.md`, enabled in `settings.json`), which applies exactly those; spot-checking
   12 of the 152 through it repaired 7, and most of the remainder were sentences that trail off
   and should not be given a full stop. So this is a real regression in the raw stream and a
   small one after the pipeline the user actually runs.

R2T2 also returns nothing where the incumbent returns a filler — `"Um."`, `"Oh."`, `"Okay."` —
on 10 clips. Nine of those are arguably an improvement for a dictation tool. One was not a
filler, and that is the shape of the risk: append-only output that declines to guess loses the
occasional real short utterance.

### What this run cannot say

Which service is more *accurate*. Nobody wrote down what was said, so 171 clips differ in words
with no way to score them. `adjudicate.md` in the run directory holds the 60 worst
disagreements with the audio extracted next to them, ready for a human; it is gitignored, and it
is the only route from here to an accuracy number.
