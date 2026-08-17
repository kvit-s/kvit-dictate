# kvit-dictate — Candidate models (Phase 0A)

**Checked: 2026-08-16.** Every claim below was resolved against a page fetched on that date;
the URL is given in-line. Nothing was installed and nothing was downloaded — this document is
the output of §3 Phase 0A steps 1–3. Step 4 (lock files) is the one remaining item, noted in §6.

The purpose of this phase is stated in plan.md §0: the PRD's three citations came from a chat
transcript with `utm_source=chatgpt.com` on every link, which is a known source of
plausible-but-fabricated model names and arXiv IDs. The failure mode this phase exists to
prevent is *me* extending that chain by inventing a nearest-plausible artifact. So:
"could not confirm" appears below wherever it is the truth, and estimated numbers are labelled
as estimates.

---

## 1. Verdict on the PRD's citations

**All three resolve. The PRD is not hallucinated.** This was not the expected outcome and it is
worth saying plainly: the caveat in plan.md §0 was correct to demand the check and wrong in its
implied prior.

### 1.1 `nvidia/nemotron-speech-streaming-en-0.6b` — **REAL, as described**

- <https://huggingface.co/nvidia/nemotron-speech-streaming-en-0.6b>
- <https://huggingface.co/api/models/nvidia/nemotron-speech-streaming-en-0.6b> (file listing)

Cache-Aware FastConformer-RNNT, 24 encoder layers, 600 M parameters, NVIDIA Open Model License.
Native support for punctuation and capitalisation. Four documented streaming configurations —
`[70,0]` = 80 ms, `[70,1]` = 160 ms, `[70,6]` = 560 ms, `[70,13]` = 1.12 s. Documented runtime is
NeMo ≥ 25.11 or Riva ≥ 2.25.0. The PRD's "chunk sizes down to 80 ms" claim is accurate.

Repo contains `.nemo`, `model.safetensors` and a `q8_0.gguf`. **No ONNX in the NVIDIA repo** —
but see §2, third-party ONNX exports exist and are the path we actually want.

**Superseded, in part.** `nvidia/nemotron-3.5-asr-streaming-0.6b` (released 2026-06-04,
OpenMDW-1.1, 1.26 M downloads) is the multilingual extension: same 600 M cache-aware
FastConformer-RNNT plus language-ID prompt conditioning, 40 language-locales, and an extra
320 ms chunk configuration. The English repo is still maintained (last modified 2026-08-05) and
carries a `nemotron-speech-streaming-jan2026` branch.
<https://huggingface.co/api/models/nvidia/nemotron-3.5-asr-streaming-0.6b>

### 1.2 Qwen3-ASR 0.6B / 1.7B and arXiv **2601.21337** — **REAL, as described**

- <https://arxiv.org/abs/2601.21337> — *Qwen3-ASR Technical Report*, Xian Shi, Xiong Wang,
  Zhifang Guo et al. (Qwen Team, Alibaba). Jan 2026.
- <https://huggingface.co/Qwen/Qwen3-ASR-1.7B> · <https://huggingface.co/Qwen/Qwen3-ASR-1.7B-hf>
- <https://huggingface.co/api/models/Qwen/Qwen3-ASR-0.6B-hf> (Apache-2.0, `transformers`)
- <https://github.com/QwenLM/Qwen3-ASR>

Two ASR models (1.7B, 0.6B) plus `Qwen3-ForcedAligner-0.6B`, 52 languages/dialects, Apache-2.0,
built on Qwen3-Omni. The PRD's "SOTA among open-source, competitive with proprietary APIs"
paraphrase matches the abstract. The arXiv ID is exactly right.

**One material fact the PRD does not mention, and it changes the plan** (§5.3 below): the
official repo states *"Currently, streaming inference is only available with the vLLM backend"*.

### 1.3 The 50-config comparison, arXiv **2604.14493** — **REAL, as described**

- <https://arxiv.org/abs/2604.14493> · <https://arxiv.org/html/2604.14493v2>
- *Pushing the Limits of On-Device Streaming ASR: A Compact, High-Accuracy English Model for
  Low-Latency Inference* — Banfic, Fan, Vaishnavi, Kemp, Choi, Ren, Shaw, Tang. v1 2026-04-16,
  v2 2026-04-19.

Benchmarks "over 50 configurations spanning OpenAI Whisper, NVIDIA Nemotron, Parakeet TDT,
Canary, Conformer Transducer, and Qwen3-ASR" and does select Nemotron Speech Streaming for
real-time English. The PRD's characterisation is accurate.

It is also, unexpectedly, the most directly useful document found in this phase, because it did
what plan.md §5.2 proposes doing: *"we re-implemented the full streaming ASR pipeline natively
in ONNX Runtime"*, as a three-graph decomposition (encoder / decoder / joiner) with stateful
zero-copy cache management, native mel extraction, and RNNT greedy decoding. Reported result:
**8.20 % average streaming WER over eight benchmarks at 0.56 s algorithmic latency, faster than
real time on CPU**, int4 k-quant, within 1 % WER of full precision; 2.47 GB → 0.67 GB.
Evaluation code at <https://github.com/nenad1002/open_asr_leaderboard>; the optimised model
ships via Microsoft Foundry Local / ONNX Runtime GenAI.

Two numbers from it that we should carry into Phase 2 as *external* reference points, not as
our own measurements:

- Qwen3-ASR-1.7B: best batch WER of the whole sweep (5.90 %) but **degrades badly chunked**
  (BSF 1.77) and runs **below real time on CPU** (RTFx 0.49).
- Parakeet TDT-0.6B-v3 across 18 chunking configurations: best chunked 9.22 % vs 6.32 % batch —
  a 46 % relative penalty. Their conclusion, which is ours too: models not designed for
  streaming pay a large price when chunked.

---

## 2. Candidate table

Columns are the ones plan.md §3 Phase 0A step 2 requires, plus context biasing because §4
compares context-enabled and context-free modes separately and a backend that cannot be biased
must not be silently scored against one that can.

**Streaming mode** uses plan.md's three values strictly:
`native-streaming` = the architecture is causal/cache-aware and the published recipe is chunked
inference; `stateful-chunked` = not designed for streaming but has an official chunked mode with
carried state; `pseudo-streaming` = a batch model driven by a sliding/overlapping window we
build ourselves.

**VRAM** figures are *estimates from published weight sizes and precision*, not measurements.
Peak VRAM is a Phase 1 metric (§4) and will be measured, not inherited from this table.

| # | Model id | Licence | Params | Runtime dep | Streaming mode | Published ONNX export | Approx VRAM | Vocab / context biasing |
|---|---|---|---|---|---|---|---|---|
| A | `nvidia/nemotron-speech-streaming-en-0.6b` | NVIDIA Open Model License | 600 M | NeMo ≥25.11 / Riva ≥2.25 | native-streaming (80/160/560/1120 ms) | not in NVIDIA repo | ~2.5 GB fp32 · ~0.7 GB int8 | none documented |
| A′ | `csukuangfj2/sherpa-onnx-nemotron-speech-streaming-en-0.6b-{80,160,560,1120}ms[-int8]-2026-04-25` | inherits A | 600 M | **ONNX Runtime via sherpa-onnx** | native-streaming, one repo per chunk size | **yes — encoder/decoder/joiner + tokens.txt** | ~2.5 GB fp32 · ~0.7 GB int8 | **no** — greedy_search only (see §5.1) |
| B | `nvidia/nemotron-3.5-asr-streaming-0.6b` | OpenMDW-1.1 | 600 M | NeMo ≥26.06 | native-streaming (80/160/320/560/1120 ms) | not in NVIDIA repo | as A | none documented; has lang-ID prompt |
| B′ | `csukuangfj2/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-{80,160,320,560,1120}ms[-int8]-2026-06-11` | inherits B | 600 M | ONNX Runtime via sherpa-onnx | native-streaming | **yes** | as A′ | no |
| B″ | `onnx-community/nemotron-3.5-asr-streaming-0.6b-onnx-int4` | MIT wrapper over B | 600 M | ONNX Runtime **GenAI** | native-streaming | **yes, int4** (+ bundled `silero_vad.onnx`) | ~0.67 GB | no |
| C | `sherpa-onnx-streaming-zipformer-en-2023-06-26` (also `-2023-06-21`, `-2023-02-21`) | Apache-2.0 (icefall/k2) | not published — small, ~70 M class | ONNX Runtime via sherpa-onnx | native-streaming | **yes, that is the distribution format** | < 0.5 GB | **yes — hotwords via `modified_beam_search`** |
| D | `openai/whisper-large-v3-turbo` | MIT | 809 M | HF `transformers` | pseudo-streaming (we build the window) | no | ~1.6 GB fp16 | prompt/`initial_prompt` only — soft |
| E | `Systran/faster-whisper-large-v3` | MIT (CT2 conversion of `openai/whisper-large-v3`) | 1.55 B | CTranslate2 / faster-whisper | pseudo-streaming | no (CT2 `model.bin`, not ONNX) | ~3.1 GB fp16 | `initial_prompt`, `hotwords` arg — soft |
| E′ | `Systran/faster-distil-whisper-large-v3` | MIT (CT2 conversion of `distil-whisper/distil-large-v3`) | 756 M | CTranslate2 / faster-whisper | pseudo-streaming | no | ~1.5 GB fp16 | as E |
| F | `Qwen/Qwen3-ASR-0.6B-hf` / `-1.7B-hf` | Apache-2.0 | 0.6 B / 1.7 B | HF `transformers`; **streaming needs vLLM** | stateful-chunked (2 s windows, 5-token fallback, 4-chunk state) — vLLM only | no official | ~1.4 GB / ~3.5 GB bf16 | **yes — context prompt**, but soft |
| G | `nvidia/parakeet-tdt-0.6b-v2` | CC-BY-4.0 | 600 M | NeMo | offline TDT — chunking costs ~46 % rel. WER (arXiv 2604.14493, v3) | community: `istupakov/parakeet-tdt-0.6b-v2-onnx`; sherpa-onnx offline export script | ~2.5 GB fp32 | via sherpa-onnx offline transducer hotwords |

Sources for the rows, all fetched 2026-08-16:
`https://huggingface.co/api/models/<id>` for A, B, B″, F(0.6B-hf), G, D, E;
`https://huggingface.co/api/models?search=sherpa-onnx-nemotron` for the A′/B′ family;
<https://k2-fsa.github.io/sherpa/onnx/pretrained_models/online-transducer/zipformer-transducer-models.html>
for C; <https://k2-fsa.github.io/sherpa/onnx/hotwords/index.html> for the biasing column.

---

## 3. Environment each candidate needs

plan.md §5.2 is ONNX-first, and Phase 0A's job is to say how much of the candidate set that
actually covers. Answer: **the entire streaming half of it.** Rows A′, B′, C and G-via-ONNX all
run on one `onnxruntime` + `sherpa-onnx` environment with **no PyTorch at all**. That is the
outcome §5.2 hoped for rather than assumed, and it is the reason the shortlist in §5 is shaped
the way it is.

| Env | Rows | What would have to be installed | Torch? | Notes |
|---|---|---|---|---|
| `envs/onnx` | A′, B′, C, G(offline ONNX) | `onnxruntime` (or `onnxruntime-gpu`), `sherpa-onnx`, `numpy`, `soundfile` | **no** | few hundred MB. Also the Phase 5 in-process C++ path — sherpa-onnx ships prebuilt Windows static/shared libs and a C++ API. |
| `envs/onnx-genai` | B″ | `onnxruntime-genai` | no | only if we want the int4 Foundry-Local export; distinct API from sherpa-onnx, so a distinct adapter. |
| *(none — existing)* | D | **nothing** | n/a | `openai/whisper-large-v3-turbo` is already in `HF_HOME`; run through `..\wan-dancer\.venv` read-only, per §5.1. Verified present on disk today as `models--openai--whisper-large-v3-turbo`. |
| `envs/ct2` | E, E′ | `faster-whisper` (pulls `ctranslate2`, `av`, `tokenizers`, `onnxruntime` for its VAD) | no torch, **but** CUDA 12 + cuDNN 9 DLLs | see the Blackwell caveat in §6. |
| `envs/nemo` | A, B, G (native) | `nemo_toolkit[asr]` + torch + `lightning` + `hydra` + a long tail — multi-GB | **yes** | **Only needed if the ONNX exports prove unfaithful.** Deciding this was the stated purpose of the ONNX column, and the answer is: NeMo need not be installed. |
| `envs/qwen-asr` | F | `qwen-asr` (transformers backend) + torch; `qwen-asr[vllm]` for streaming | **yes** | vLLM on Windows is the blocker, not the model — see §5.3. |

Local environment facts confirmed today, not assumed:

- `HF_HOME=c:\apps\llama-swap\gguf\hf` is set globally; the hub cache contains
  `models--openai--whisper-large-v3-turbo` and `models--Systran--faster-whisper-base.en`, matching
  plan.md §5.2.
- `uv` is at `C:\Users\sk\.local\bin\uv.exe`.
- Before creating any env, set `UV_CACHE_DIR` to a directory **on D:** — uv hardlinks from cache
  into the venv and hardlinks cannot cross volumes, so a C: cache with D: venvs full-copies
  every package (plan.md §5.2.4).

### Nothing is to be installed into `..\wan-dancer\.venv`

Stated explicitly because plan.md §5.1 reverses revision 1 on exactly this point and the
reversal is load-bearing. `D:\projects\wan-dancer\.venv` is **read-only for this project**. We
run its existing Whisper baseline (row D) and install nothing — not `faster-whisper`, not NeMo,
not `qwen-asr`, not an "obviously compatible" pin, not under `--dry-run` justification. It is
another project's working GPU stack containing `sageattention` compiled against that exact
torch, and a clean dependency resolution is not evidence that an added package leaves an
untested import or runtime path intact. Every backend in the table above that needs anything at
all gets its own locked env under `envs/`, even where versions look compatible. Disk is cheap;
that venv is not reproducible on demand.

---

## 4. Selected for Phase 1

Four adapters, three environments, one of which already exists.

| Pick | Role | Why |
|---|---|---|
| **1. Nemotron streaming EN 0.6B via sherpa-onnx ONNX** (A′, all four chunk sizes, fp32 and int8) | **the streaming finalist candidate** | Only candidate that is natively streaming, has a *published* ONNX export in a runtime with a C++ API, and has independent third-party evidence (arXiv 2604.14493) that it wins this exact comparison. Its ONNX form is simultaneously the Phase 2 bake-off entry and the Phase 5 in-process C++ path, so the winner would already be in the runtime we eventually want. The per-chunk-size repos map one-to-one onto §4's `chunk_ms ∈ {80,160,320,640}` sweep. |
| **2. Nemotron-3.5 streaming 0.6B via sherpa-onnx ONNX** (B′) | same adapter, one model swap | Newer, adds a 320 ms configuration that lands closer to §4's 320 ms cell than the English model's 160/560 gap, and is the version NVIDIA is actively developing. Kept as a variant, not a separate workstream — if it is not measurably better on the technical corpus, drop it, since English-only is our entire scope and multilingual capacity is dead weight. |
| **3. Streaming zipformer EN via sherpa-onnx** (C) | **the context-biasing arm** | The *only* natively-streaming candidate that supports vocabulary biasing (transducer hotwords via `modified_beam_search`). §4 requires context-enabled and context-free modes to be compared separately; without this row the context-enabled column has no native-streaming entrant at all. Expected to lose on raw WER — that is fine, it is the reference point that tells us what biasing is worth. |
| **4. `whisper-large-v3-turbo` in the existing venv** (D) | **baseline, zero install** | Already cached, already proven on this GPU by `wan-dancer\notes\transcribe.py`, costs nothing, and Phase 1's adapter is a near-copy of that script. It anchors the accuracy scale and is the natural partner for the manual `Win+H` comparison in §4. Pseudo-streaming, so per plan.md its latency numbers are *not* comparable to rows 1–3 and must be reported in a separate block. |

**Second tier — batch-only in Phase 1, excluded from the Phase 2 streaming sweep:**

- **Qwen3-ASR-1.7B / 0.6B (F)** — run batch, through `envs/qwen-asr`, purely to measure the
  accuracy ceiling and to see whether prompt-based context biasing recovers `llama.cpp` where
  the streaming models write `llama dot cpp`. That single measurement is worth the env. It is
  *not* a streaming candidate here; see §5.3.
- **`faster-whisper` large-v3 / distil-large-v3 (E, E′)** — batch accuracy reference and the
  cheapest CPU-side fallback. Deferred until rows 1–4 have run: it needs its own env and the
  Blackwell issue in §6 is a real, if worked-around, cost.
- **`parakeet-tdt-0.6b-v2` (G)** — real and good, but offline. arXiv 2604.14493 already measured
  a 46 % relative chunking penalty on its v3 sibling, so running our own chunked sweep would
  spend Phase 2 budget re-deriving a published negative. Batch reference only.

**Not selected:** the `q8_0.gguf` files in the NVIDIA repos (no runtime in our stack consumes
them — see §6); `RedHatAI/nemotron-speech-streaming-en-0.6b` (mirror, adds nothing);
`onnx-community/...-int4` (B″) is held in reserve — attractive at 0.67 GB, but it is a second
ONNX API (`onnxruntime-genai`) and a second adapter for a size saving that does not matter on a
32 GB card. Revisit in Phase 5 if a CPU-only build becomes interesting.

---

## 5. Findings that change the plan

### 5.1 The streaming finalist cannot be context-biased

sherpa-onnx's Nemotron online transducer supports **`greedy_search` only**; hotwords require
`modified_beam_search`, which the Nemotron path explicitly rejects
(<https://github.com/k2-fsa/sherpa-onnx/issues/3572>, open). Nor does either NVIDIA model card
document word boosting. Riva has word-boosting features generally, but I could not confirm
today that they apply to this model, and I am not going to assert it.

So the likely shape of the §4 comparison is: **the fastest, most accurate streaming candidate is
context-free, and the context-enabled candidates are either slower (Qwen) or less accurate
(zipformer).** That is a genuine trade-off to measure, not a gap to paper over. Concretely:
the technical-term F1 metric — the one the selection rule maximises — is exactly where biasing
should help most, and our leading candidate cannot use it. Phase 1 must therefore report
technical-term F1 for row 1 context-free against row 3 context-enabled *and* context-free, or
the comparison is uninterpretable.

If that trade-off lands badly, the fallback is not a different ASR model but a post-ASR
vocabulary correction pass over the committed-text stream — which is second-pass correction by
another name, and stays deferred per plan.md Phase 2.5 until the error distribution is measured.

### 5.2 ONNX-first survives contact with reality, and NeMo need never be installed

The ONNX column was specified to decide two things (plan.md §3 Phase 0A step 2). Both are now
decided: Phase 5 in-process C++ inference is feasible for the leading candidate, and **NeMo does
not need to be installed at all** unless an export turns out to be unfaithful. That removes the
largest single install from the plan.

### 5.3 Qwen3-ASR streaming is effectively unavailable on Windows

The PRD proposes Qwen3-ASR-1.7B as a second-pass corrector and implies it is a viable streaming
alternative. The technical report does describe native streaming (2 s chunks, 5-token fallback,
state over the last 4 chunks, via the dynamic attention window). But
<https://github.com/QwenLM/Qwen3-ASR> states *"Currently, streaming inference is only available
with the vLLM backend"*, and vLLM has no supported native Windows build. Combined with arXiv
2604.14493's measurement that chunked Qwen3-ASR-1.7B degrades to BSF 1.77 and runs below real
time even on CPU, the honest conclusion is: **Qwen3-ASR is a batch-mode accuracy reference for
this project, not a streaming candidate.** WSL2 would be a route, but adding a WSL hop to the
latency budget of a dictation tool defeats the purpose.

### 5.4 The PRD's headline architecture is essentially confirmed by an independent source

`hotkey → mic → VAD → Nemotron streaming → inject` is the same conclusion arXiv 2604.14493
reached from a 50-configuration sweep, arrived at independently. That is a stronger position
than Phase 0A expected to be in. It does not change the plan — plan.md deliberately says "the
selected streaming finalist" and keeps the candidate list as data — but it does mean the
shortlist above is unlikely to be overturned by Phase 1, and Phase 1's real value is the
technical-vocabulary corpus and the ICER instrumentation, not the model choice.

---

## 6. Could not verify / open questions

Listed rather than resolved, per the house rule. None of these gate Phase 0B.

1. **`onnxruntime-gpu` on Windows for sm_120 (Blackwell / RTX 5090).** Could not confirm that
   the official PyPI `onnxruntime-gpu` wheel ships sm_120 CUDA kernels for Windows.
   <https://github.com/microsoft/onnxruntime/issues/27875> asks this question directly, was
   opened 2026-03-27, and closed **stale with no maintainer answer**. Third-party reports and
   community rebuilds (e.g. `Natfii/onnxruntime-gpu-blackwell`, ORT 1.24.1 + CUDA 13.1) imply
   the official wheel falls back to CPU on Blackwell, but I have not verified that on this
   machine and will not assert it.
   **Why it does not gate anything:** arXiv 2604.14493 measured this exact model faster than
   real time *on CPU* with 0.56 s algorithmic latency, and this machine has 28 threads. The
   CPU EP is the baseline plan for rows 1–3; GPU is an optimisation. Resolve empirically in
   Phase 1 by checking `get_available_providers()` and timing both EPs, and treat DirectML as
   the second option if CUDA is unavailable.
2. **CTranslate2 on Blackwell.** Multiple third-party reports of
   `CUBLAS_STATUS_NOT_SUPPORTED` on RTX 50-series with int8 compute, worked around by forcing
   `compute_type="float16"`. Not verified here. Cost is a constrained precision choice for
   rows E/E′, which are second-tier anyway.
3. **Zipformer streaming parameter counts.** The sherpa-onnx model pages publish no parameter
   counts. The ~70 M figure in the table is my inference from the icefall recipe class, not a
   published number. Replace with measured file sizes in Phase 1.
4. **The `q8_0.gguf` files in both NVIDIA Nemotron repos.** NVIDIA ships them; I could not
   determine which runtime consumes them (whisper.cpp does not; llama.cpp has no RNNT ASR path
   that I could confirm). Possibly for a NeMo/Transformers-side quantised load. Open.
5. **Riva word boosting for Nemotron streaming.** Riva documents word boosting as a platform
   feature; whether it applies to this specific model — and whether it is reachable without
   deploying Riva, which is a container-scale dependency we will not take — is unresolved. See
   §5.1.
6. **Nemotron licence terms.** The NVIDIA Open Model License (row A) and OpenMDW-1.1 (row B) are
   recorded by name from the model metadata. I have not read either licence text in full. Do
   that before anything ships; it is irrelevant to the experimental phase.
7. **Phase 0A step 4 — per-backend lock files — is not done.** It requires network dependency
   resolution, which is outside the "install nothing, download nothing" scope of this pass.
   Generate `envs/onnx` and `envs/ct2` locks with `uv lock` under `server/` in the first
   installing session, with `UV_CACHE_DIR` on D:. `envs/nemo` should not be generated at all
   unless §5.2's conclusion is overturned.

---

## 7. Reproduction

Every fact above came from one of these, fetched 2026-08-16:

- `https://huggingface.co/api/models/<id>` — authoritative for licence, tags, `library_name`,
  `lastModified`, and the exact file listing. Preferred over the rendered model page, which is
  prose and easier to misread.
- `https://huggingface.co/api/models?search=sherpa-onnx-nemotron&limit=50` — the A′/B′ family.
- `https://huggingface.co/<id>/raw/main/README.md` — model cards for chunk configurations,
  runtime versions, and architecture.
- `https://arxiv.org/abs/2601.21337`, `https://arxiv.org/abs/2604.14493`,
  `https://arxiv.org/html/2604.14493v2`.
- `https://github.com/QwenLM/Qwen3-ASR`, `https://github.com/k2-fsa/sherpa-onnx/issues/3572`,
  `https://github.com/microsoft/onnxruntime/issues/27875`.
- `https://k2-fsa.github.io/sherpa/onnx/hotwords/index.html` and
  `.../pretrained_models/online-transducer/zipformer-transducer-models.html`.

Local state was read with PowerShell against `HF_HOME`; no venv was created, activated, or
modified, and `..\wan-dancer\.venv` was not written to.
