# progress — kvit-dictate

Written 2026-08-21. Commit `e800028` on `main`.

Read with `plan.md` (the specification), `bench/CANDIDATES.md` (model picks),
`docs/supported-targets.md` (measurements + open questions §9) and
`tools/kvit-speech/README.md` (the local model's measurement record). This file is only: what
works, what does not, and what to do next.

---

## 1. Where we are

**Dictation works end to end against a real ASR model.** Press `Win+Y` in Notepad3, speak, and
your words are transcribed by the Qwen3-ASR server on `strixhalo.lan` and typed into the document,
with a live preview at the caret that follows it as text commits.

That sentence became true today. Before it, every word the product had ever produced came from a
scripted mock.

The thing that made it cheap was the **HTTP backend**: `server/kvit_asr/backends/openai_http.py`
posts audio to an OpenAI-compatible `/v1/audio/transcriptions` endpoint and is **stdlib-only**
(`urllib`, `wave`). No environment to build, nothing to download, no GPU stack to reconcile — it
loads under the same bare CPython the mock uses. `envs/` still does not exist and no model has
been downloaded.

**The one bug that made it unreliable is fixed, and confirmed by a human.** A long sentence used
to abort partway and lose its tail. Diagnosed from the log and the two sources and fixed without a
repro, then verified in a real dictation on 2026-08-18 at 22:09 — 47 seconds into Notepad3, seven
utterances committed, no violation. The two sessions it used to fail in died after about five. §3
has the mechanism.

**Since then the far end has moved under it, twice over** (2026-08-21): strixhalo switched to
vLLM and got 4–5× faster, and grew a Silero VAD route. Both are now used — see §3b. Together they
retired the two standing quality questions, and **Silero will not be run locally** (§6).

**And there is now a second real model that needs no far end at all** (2026-08-21, later):
`windows-speech` runs Microsoft's on-device speech model — quantized Whisper Small, an optional
Windows component — entirely on this machine. It is the first configuration that keeps plan.md
§2's "no network access during dictation" in the letter and not only the spirit, so §6's standing
question *"what does the product do when the far end is unreachable?"* now has an answer to choose
rather than only a problem to state. It is slower than the LAN (≈3.0 s for a 5 s utterance against
strixhalo's ≈550 ms) and its API is experimental. §3d has the whole thing.

---

## 2. What works, verified by a human

* **The whole pipeline**: hotkey → mic → VAD → HTTP transcription → stability → injection →
  overlay. Notepad3, `console=0`, `SendInput`.
* **Inline overlay at the real caret.** Notepad3 exposes a system caret, so source A wins at 0.95
  and the overlay tracks the true insertion point — logged moving 2738 → 2815 → 2918 → 3021 across
  one sentence while the pixel-diff estimator's 0.66–0.90 was correctly rejected in favour of it.
* **The floating pill**, as the low-confidence fallback, bottom-centre of the target's monitor.
* **Refusals**: password fields refuse before injecting.
* **Four targets characterised** (observations, not measurements — `docs/supported-targets.md`
  §5.10): Notepad3 clean; Win11 Notepad garbles paced `SendInput`; VS Code's terminal clean;
  Windows Terminal detected as a console, text lands, overlay degrades to the pill.

### Test suites

```
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
build-windows-msvc-debug\bin\Debug\kd_tests.exe        # 331 tests / 36005 checks
cd build-windows-msvc-debug && ctest -C Debug          # 15 suites, none disabled

python -m unittest discover -s server/tests -t server  # 419 tests   (from the repo root)
python -m unittest discover -s bench/tests  -t .       # 129 tests   (from the repo root)
```

Use `C:\Python312\python.exe`. The `python` on PATH in Git Bash is a conda 3.9 and will fail.
Kill any running `kvit-dictate.exe` before rebuilding — it holds the link output *and* the
single-instance mutex, so a stale one produces `LNK1168` **and** a new instance that exits
immediately, which is two symptoms of one cause.

### Runtime configuration

`%LOCALAPPDATA%\kvit-dictate\settings.json` — currently `model_id: "qwen-asr-http"`,
`overlay.mode: "inline"`, `log_level: debug`. A backup of the pre-real-model settings is beside it
as `settings.json.before-real-model`. The log at
`%LOCALAPPDATA%\kvit-dictate\logs\kvit-dictate.log` is the first thing to read after any session.

**Two different things control "how often does text appear", and they live in different files.**

1. **How often audio is transcribed** — the utterance boundary. `settings.json`'s
   `session.endpoint_silence_ms` is only a *hint* (`session.cpp:752` says so; `close_utterance()`
   flips a bool and audio keeps flowing). The authoritative boundary is the sidecar's, so the knob
   is `--endpoint-silence-ms` in **`models.json`**, now **1500 ms** for both qwen entries and both
   windows-speech entries (was 800, which split a sentence at every breath). The client-side value
   is kept at 1500 only so the two do not read as disagreeing.
2. **How often text is typed** — `settings.json`'s `cadence.mode`, now **`end-of-session`**. See
   §3c. The two are independent: the sidecar still decodes about a sentence at a time and still
   uploads at most an 8 s window, whatever the client does with the results.

Model entries that matter:

| id | what it is |
|---|---|
| `qwen-asr-http` | LAN Qwen3-ASR, re-transcribes once a second → live preview that revises. Used to trip the stability abort; fixed in `89d012e` and confirmed working (§3). Commits once per utterance, at the endpoint. |
| `qwen-asr-http-finalize` | one request per utterance, no live preview. Cannot trip it. The reliable one. |
| `mock` | scripted; the only entry that needs nothing at all. Still the way to test the C++ side in isolation. |
| `windows-speech` | **local, no network.** Microsoft's on-device model (Whisper Small int8) via `tools/kvit-speech`. Finalize-only — one decode per utterance. Slower than the LAN; see §3d. |
| `windows-speech-preview` | the same, with the hop on so the overlay revises. Lags on purpose; it exists so the lag can be seen rather than argued about. |
| `whisper-large-v3-turbo` | zero-install baseline: the weights (1.6 GB) and the venv both already exist on this machine. Never run. |

---

## 3. The bug that mattered — diagnosed, fixed, and confirmed

**A long sentence aborted mid-way and lost its tail.** Reproduced twice, on 2026-08-18 at 15:53
and 17:56. Fixed in `89d012e`, confirmed by a human dictation at 22:09 the same day: 47 seconds
into Notepad3, seven utterances committed at their endpoints, no violation and no `session error:`
line. Both failing sessions had died after roughly five seconds.

### What it actually was

Diagnosed without a repro, by reading both sides and replaying the sequence offline. The chain:

1. `qwen-asr-http` re-transcribes the **whole utterance** every second. A growing-window batch
   model does not extend its previous hypothesis, it replaces it — so once it can see where the
   sentence ended it re-cases the opening word and inserts the punctuation it was unsure of, and
   `so I was thinking we` comes back as `So I was thinking, we`.
2. `StabilityTracker` had already published a mark covering those bytes. §8.2 binds a new
   hypothesis against the *previously published* mark, so `_evaluate`'s step 0 raised
   `STABLE_PREFIX_MUTATED` — correctly, and with no value of `stable_bytes` that would have made
   the message conforming.
3. `Session._fail_session` turned that into `error{code:decode_failed, fatal:false}` + `stopped`.
4. `session.cpp`'s non-fatal `error`-frame branch aborted the session — and set **no** `o.error`.

**The diverging offset is 0.** The *first word* was revised, so no non-zero mark was ever safe.

### Two things §3 used to say that were wrong

* **It was not the C++ §8.2 check.** That check (`session.cpp`, the `check_stability` call) never
  ran, and neither did the commit engine's. Both aborts came through the error-frame branch four
  paragraphs up. The tell was in the log all along: `31 byte reason` is the length of the fixed
  string `committed text was contradicted`, which only `main.cpp`'s `SurfaceWarning` handler
  passes — `perform_commit` passes a much longer one.
* **Reading the new `session error:` line would not have worked.** That branch populated no
  `o.error`, so the next occurrence would have logged exactly what the last two did. Fixed in
  `4805a1d`; the branch now carries the child's code, message and session_id out.

### The fix, and why the other candidate could not have worked

`SessionConfig.publish_stable_partials`, derived in `ConnectionSession._start` from the backend's
own declared `pseudo-streaming` mode. A backend in that class publishes **no mark before the
`final`**; the tracker is not fed during partials at all.

Tuning `stability_n/m/hold_ms` — the first fix this file proposed — **cannot** work, and the
offset is the proof. The three knobs delay the mark; delaying a mark that will be contradicted at
byte 0 changes when the abort happens, not whether it does.

**The live preview survives.** `partial` still carries the full revising text and the client
renders everything above the mark as pending (plan.md §1.5), so with the mark pinned at 0 that is
the whole hypothesis. The words still appear as they are spoken; they land in the document at the
endpoint instead of a few hundred ms earlier. This is *not* `qwen-asr-http-finalize`, which has no
live preview at all.

`CommitEngine::Result::salvage` (7 tests) is still correct and still not on this path.

---

## 3b. The far end: linear and fast, and it now answers VAD questions

**Retraction, kept because it cost a day's conclusions.** This section once reported a latency
cliff between 8 s and 12 s of audio and concluded `window_ms` could never be raised. That was an
artifact of the harness: the sweep used synthetic harmonic noise, which drives the model into a
repetition loop until it hits its token cap (~7 s to return the 9 characters `'Whoa! Who'`). The
data said so and went unread — the *slow* responses returned the *least* text. **Measure this
endpoint with real speech or not at all.**

**Measured 2026-08-21, real speech** (SAPI, sliced from one 57 s utterance), after the server moved
to vLLM:

| audio | 2026-08-21 (vLLM) | before | ×realtime |
|---|---|---|---|
| 8 s | **237 ms** | 825 ms | 34× |
| 16 s | **344 ms** | 1501 ms | 47× |
| 30 s | **541 ms** | 2587 ms | 55× |

Linear, no discontinuity. `/health` reports `{"backend": "vllm", "vad": true}`.

### What bounds the window

Throughput, not a ceiling. Each hop re-uploads and re-decodes the **whole** window, so
`window_ms/50 + vad` must stay well under `chunk_ms`. Falling behind overflows the sidecar's
bounded audio queue (3000 ms) into a **fatal `overload_audio`**, so this wants margin.

Now **`window=30000`, `hop=2000`** — **~36% duty** including the VAD call, against ~73% before,
with nearly **four times** the window. A 30 s window makes the frozen-prefix stitch effectively
unreachable in normal speech; `window_slide` fired three times in two sessions at 8 s. If it ever
returns, raise `window_ms` **and `chunk_ms` together**.

### The VAD route, and why it is a correctness fix

`POST /v1/audio/vad` (Silero via pysilero-vad) answers whether a clip contains speech, in 50–176 ms
for 0.5–30 s. `vad_gate=true` asks it **before** transcribing.

This is not tidiness. Handed non-speech, the model **invents** — re-confirmed live 2026-08-21:

| input | model returns |
|---|---|
| 500 ms digital silence | `"Yeah."` |
| 2000 ms digital silence | `"Okay."` |
| key-click transients | `"Hi."` |

Under end-of-session cadence (§3c) those are typed into the document along with everything real,
and the log holds two 5-byte commits — the exact length of `"Yeah."` — both of which were Esc'd.
The VAD route rejects all three cleanly (`speech: false`) and passes real speech.

* **In the backend, not `kvit_asr.vad`.** That module is a frame-wise *streaming endpointer*
  deciding when an utterance ends, 32 ms at a time; this is a *batch* question about a finished
  buffer. Different shapes — so the gate does not replace the local VAD, it adds a
  better-informed opinion at the one moment that matters.
* **Fails open**: missing route, timeout, bad body → transcribe anyway. Losing real speech is
  worse than an occasional invented word, and a VAD outage must not silence dictation. It latches
  after the first refusal so a 2 s hop cannot log a warning every 2 s.
* **This is the silero question, answered without a local install**, and §6 records the decision
  not to install one. The server runs the same model with the operator's own tuning.

---

## 3c. Deferred commit (`cadence.mode: "end-of-session"`)

Text accumulates for the whole session and is typed once, at the end, in a single injection.
Currently **on**. `Cadence::Immediate` is the old behaviour and is still the default in code.

* **It costs nothing in audio.** Utterance boundaries and injection cadence were already fully
  decoupled — the sidecar decodes a sentence at a time into an 8 s window regardless (§3b).
* **Implemented across two files on purpose**, and the header says so: the engine's cadence gate
  covers only the partial path (`cadence_allows`, one call site — `on_final` never asks), so the
  engine suppresses early flushes and `App::flush_deferred_commits` does the accumulating. The
  engine still emits its op at each `final`, because a caller that buffers needs something to
  buffer.
* **The old `cadence` knob was dead** for this backend before this: every op comes from
  `on_final`, which does not consult it. Worth knowing before trusting any other §1.4 knob.

What happens to the buffer when the session ends:

| ending | what happens |
|---|---|
| hotkey again, click, keystroke, silence | **typed into the document** — all go through `finalise_session` |
| **focus moved to another window** | **clipboard**, plus a tray warning. It cannot be typed: `validate_target` requires the target to still be the foreground window, and the injector never restores focus because `SendInput` types into whatever *is* focused. |
| Esc | **discarded**, deliberately |
| crash / fatal error | lost, logged |

**The visible tail is 8 lines** (`kMaxLines`, raised from 3) and there is **no scrollback** — a
session-long buffer sits behind that window unseen. Front-elision means it always shows the newest
lines, so it reads as scrolled to the bottom. The overlay is on the floating pill because 8 lines
at the caret would cover the document being dictated into; the pill also no longer resizes to its
contents, so it stops jittering on every revision.

---

## 3d. The local model — `windows-speech`, added 2026-08-21

**Dictation no longer requires another machine.** `windows-speech` runs Microsoft's on-device
speech model through `Microsoft.Windows.AI.Speech`. Switching between it and the LAN is what it is
for every other model: `model_id` in `settings.json`. No C++ changed — `models.json`'s
`interpreter` + `module` + `args` already carried everything this needed.

Verified through the real `Backend` interface against a 7.5 s synthesized utterance: load 2802 ms,
one decode at the endpoint in 3486 ms, transcript exact. **Not yet verified by a human dictating
into a document** — that is item 1 of §6.

`tools/kvit-speech/README.md` carries the full measurement record. The five things that decide how
it is used:

**1. It is Whisper Small, int8.** The component package contains
`whisper_small_encoder.quant.onnxe` (107 MB) and `whisper_small_decoder.quant.onnxe` (181 MB). So
"uses components from OpenAI Whisper" means it *is* Whisper, and every Whisper caveat in this file
applies to it — including §4's invented commits over silence. Measured here: two seconds of
**digital silence** returned `Rwy'n cael ei wneud i'r unrhyw ychydig.`, a fluent Welsh sentence
from an all-zero buffer. The backend carries its own `silence_peak_threshold` gate behind the
server's VAD, and `test_windows_speech.py` asserts on the *request counter* rather than the text,
because that assertion has to fail even when the invention is plausible.

**2. It is slower than the LAN, and the shape of the cost is what matters.** Decode is
≈ **1350 ms fixed + 0.30× real time** on this i7-14700K.

| audio | 1 s | 3 s | 5 s | 8 s | 30 s |
|---|---|---|---|---|---|
| decode | 1668 ms | 2203 ms | 2984 ms | 3686 ms | 10197 ms |

strixhalo does 5 s in ≈550 ms (§3b), so this is ~5× slower. The **fixed floor** is the part with
consequences: a one-second hop needs a 1.7 s decode every second, so a live preview cannot keep up
and falls further behind within a sentence. `finalize_only` is therefore the default, which is the
same shape as `qwen-asr-http-finalize` and invisible under end-of-session cadence (§3c).

**3. The API is experimental, and the documentation is wrong about that.** Microsoft's page gives
the prerequisite as "WinAppSDK 1.7.1 or later". In fact `Microsoft.Windows.AI.Speech` exists in no
stable Windows App SDK — 2.4.0 ships the Text, Imaging, Video, ContentSafety and MachineLearning
projections and no Speech projection at all. `tools/kvit-speech` pins `2.3.2-experimentala` and
carries the runtime beside the exe, so this does **not** require a preview runtime system-wide.
`CS8305` warnings on every build are the API saying so; they are left visible.

**4. Package identity is not required, despite the documentation saying it is.** The docs demand
an MSIX with the `systemAIModels` capability, which would have forced identity on the whole
process tree. Measured: `GetReadyState()` and `EnsureReadyAsync()` both work unpackaged, and only
`TryCreateAsync()` fails — with `0x8007007E`, `ERROR_MOD_NOT_FOUND`. The implementation DLLs ship
*inside* the component package's `winml\` folder and an unpackaged process has no package graph to
resolve them through. Adding that folder to the DLL search path is the entire fix. **This is the
most likely thing here to break on a future Windows update**; the symptom is exactly that HRESULT
and the fallback is a sparse package identity for the helper alone.

**5. The WinRT is in a separate process, and that is not incidental.** `tools/kvit-speech` is a C#
helper the sidecar spawns and talks to over a small framed protocol. It is there so the sidecar
package stays stdlib-only — a WinRT projection and a Windows App SDK bootstrapper inside
`C:\Python312` would end that rule for *every* backend, not just this one. It also means the
blast radius of an experimental API is one directory.

It hijacks stdout before loading any speech DLL, because **those DLLs print to stdout**
(`[loader] INT8 zp size mismatch: …` on every startup). That is precisely the hazard
docs/protocol.md §2.1 cites when it refuses stdio for the main protocol, except here it is
observed rather than hypothetical.

### The streaming API was measured and rejected for this seam

`StreamingRecognition` + `SpeechAudioProvider` does let the caller push its own PCM rather than
letting the API open the microphone — which is the property kvit-dictate needs, since capture,
VAD and gating already live here. It was not used anyway, because its output shape fights
docs/protocol.md §8.1:

```
off=0     dur=4000   "The quick brown fox jumps over the lazy dog. This"
off=3960  dur=4000   " is a test of the Windows on-device speech recognition model."
off=7960  dur=1120   " of the Windows on Device Speech Recognition Model."
```

`Recognized` emits **overlapping four-second windows whose text repeats across the seam, recased**.
`Recognizing` never fired at all and `IsFinal` was never true. §8.1 requires cumulative text, so
using this means a de-overlap heuristic whose failure mode looks exactly like the stability
violations §8.3 exists to reject — and §3 is the record of what one of those costs to diagnose.
Batch returns one clean, correctly-cased, correctly-punctuated string per window, which is what
`PseudoStreamingBackend` is built around.

Two facts about it recorded so nobody rediscovers them: `StartContinuousRecognitionAsync()` must
**not** be awaited before pushing audio (it runs for the life of the stream; awaiting it first
deadlocks), and `provider.Dispose()` — not `StopContinuousRecognition()` — is the end-of-stream
signal.

---

## 4. Also true, and smaller

* **The local endpointer stays as it is, and that is now a decision rather than a default.**
  Silero locally was the standing open item; §4b measures it and §6 closes it for good — the
  energy scorer is over-inclusive, not lossy, so a local Silero buys boundary precision only. The
  unverified "triggers on keyboard noise" claim also stays retired: over-inclusive is exactly what
  that claim predicted, and §3b's gate is the answer to it either way.

* **The model hallucinates over silence**, measured: 500 ms of digital silence returns `"Yeah."`,
  2000 ms returns `"Okay."` No guard is implemented on purpose — the endpoint returns a bare
  string with no timestamps to filter on, and a phrase blocklist would silently delete words a
  user really said. VAD gating upstream is the real fix, which is what makes the VAD above matter.
  **Re-confirmed 2026-08-21** against the live endpoint, and it is worse than "silence": 500 ms of
  silence returns `"Yeah."`, 2000 ms returns `"Okay."`, and synthetic key-click transients return
  `"Hi."` — both 5-byte strings, and the log contains two 5-byte commits (2026-08-21 00:12–00:13),
  both of which the operator pressed Esc on. Consistent with hallucination; not proof, since a real
  5-byte word is equally possible. The transcript-log test above is what would settle it.
* **Clicking or typing ends a session.** By design (plan.md §1.8). The sequence is: focus the
  target, *then* `Win+Y`, then do not touch the mouse. The log now names the gesture.
* **`bench/CANDIDATES.md` §4 is contradicted by testing.** It names Nemotron and zipformer as the
  streaming finalists; the operator has measured both as much worse quality than Qwen. Not yet
  recorded in that file. It matters more than a note: if the quality winner is a *batch* model,
  then reconciling revisions is the permanent architecture, not a bring-up compromise waiting for
  a streaming model to make it moot.

---

## 4b. Which VAD does what

Six things could be called "the VAD". Three of them run.

| | where | what it decides | status |
|---|---|---|---|
| **energy/ZCR endpointer** | sidecar, `vad.py`, frame-wise on 32 ms frames | **when an utterance ends** (`--endpoint-silence-ms`, 1500 ms) and what reaches the decoder | **running** — this is "the local VAD" |
| **`/v1/audio/vad`** (Silero) | strixhalo, batch, per window | **whether a window is speech at all**, before it is transcribed (§3b) | **running** — added 2026-08-21 |
| **`silence_peak_threshold`** | `windows_speech.py`, per window | whether to spend a local decode at all | **running** — added 2026-08-21, and it is a *correctness* gate, not a saving (§3d) |
| Silero ONNX in-process | sidecar, `SileroOnnxScorer` | would replace the energy scorer above | **not happening** — decided, see below |
| the Windows model's own VAD | inside the speech component (`vad.all.onnxe`, `vadapi.dll`) | its internal segmentation | **runs, and is not ours** — no API exposes it, no setting reaches it, and it did not stop the Welsh sentence over silence (§3d) |
| Media Foundation Voice Capture DSP | `MFPKEY_WMAAECMA_FEATR_VAD`, `AEC_VAD_*` | would be a frame-wise speech/non-speech flag in the C++ client | **not needed** — see below |

**On the Voice Capture DSP VAD.** It is real, it is public, and it is the wrong tool here. Three
reasons, in order of weight. It is a *frame classifier*, so it answers "is this frame speech?" and
not "has the speaker stopped?" — the endpointing state machine in `vad.py` would still have to
exist, and would still be the thing with the bugs. It is a property of the Voice Capture DMO,
which means adopting it means routing capture through that DMO and inheriting its AEC and its
resampling, a change to `audio_capture.cpp` far larger than the thing it buys. And the measurement
in §4b already says the local scorer is *over*-inclusive rather than lossy, so a better frame
classifier is not what is missing. Recorded as considered and declined, not as unexamined.

They are not alternatives. The first is a *streaming endpointer* answering "has the speaker
stopped?" incrementally; the second is a *batch classifier* answering "is there speech in this
buffer?" about a finished window. Only the first can drive utterance boundaries; only the second is
good enough to be trusted about content.

### Do we need Silero locally too? No, and that is now a decision — measured 2026-08-21

It is the same model the server runs, so a local copy could only add one thing: *streaming*
endpointing, the remote route being batch. The question is therefore narrow — **is the energy
scorer losing speech?** Its false *positives* no longer reach the document (§3b's gate catches
them), but audio it refuses to pass never reaches the backend at all and nothing can recover it.

57.4 s of real speech through both:

| | result |
|---|---|
| server Silero | 48.93 s of speech, 12 segments |
| local energy scorer | **58.70 s passed to the decoder**, 8 utterances — **120%** |

**It is over-inclusive, not under-inclusive.** It passes more than the whole file (pre-roll padding
re-emits lead-in) and loses nothing. Identical with and without lead-in silence, so the
calibration-window worry in its docstring did not materialise on a hotkey-style opening either.

Two consequences:

* **A local Silero would buy nothing that matters, so it is not being installed** (§6). What it
  would improve is boundary *precision* — 8 utterances against Silero's 12 segments — which is
  punctuation granularity, and invisible under end-of-session cadence. Not worth putting
  `onnxruntime` on this machine.
* **It also explains the `"Yeah."` commits.** Passing ~everything means digital silence was
  reaching the decoder unimpeded, and the model invents on silence (§4). The local scorer was
  never filtering content; §3b's gate is what does it now.

Caveat, since this file has been burned once today: clean synthesised speech in a silent file. A
real microphone with a fan or a conversation in the room is the case not covered, and it is the
one the docstring's "detects sound, not speech" is really about.

**The client (C++) has no VAD at all**, which was not the design — see §8.

---

## 5. Phase 0B, and why it is parked

15 of 180 cells (`kd_probe.exe --status`). `docs/supported-targets.md` has the detail; the
headline is that the phase produced a **negative** result, which plan.md §3 explicitly calls a
success for the phase rather than a failure:

* Source C (pixel differencing) is **accurate on a clean append** — 0.07 line heights on both
  measured applications — and **wrong by ~10 line heights on a mid-line insertion**, because text
  to the right reflows.
* **Its confidence score cannot tell those two apart.** Twelve cells on the calibration target
  supported a separating threshold; the first two cells of a real application destroyed it
  (`A10/S02`, wrong by 10.27 line heights, scores 0.3956 — *above* two accurate cells).
  §9 question 7 has the full argument.
* Therefore plan.md §1.5's deterministic degradation gate is **written and tested but deliberately
  not called** (`kd::overlay_mode_for`, `tests/test_overlay.cpp`, 12 tests). `App::apply_overlay_mode`
  carries the reason at the call site. This is the one piece of "tested but uncalled" code in the
  tree and it is flagged in four places on purpose.
* A caret survey found **only Notepad3** exposes a system caret among ~15 running applications —
  not Chrome, Edge, VS Code, Explorer or Windows Terminal. So the targets that would benefit most
  from inline are exactly the ones where it is least reliable.

**Parked deliberately.** The remaining Phase 0B work needs either an operator annotating frames or
machine-state changes, and none of it is on the path to a working product. It is also where several
days went that could have gone into §1. Do not resume it without a reason connected to a user-visible
problem.

---

## 6. What to do next

**Decided, not open — Silero will not run locally.** The sidecar keeps the stdlib energy/ZCR
endpointer. `onnxruntime` is not going on this machine and `SileroOnnxScorer` stays unconfigured.
The reasoning is measured in §4b: the energy scorer is *over*-inclusive (it passed 120% of the
speech the server's Silero found, losing nothing), so the only thing a local copy would buy is
utterance-boundary precision — punctuation granularity, invisible under end-of-session cadence.
The job that actually mattered, keeping invented words out of the document, is done by
`/v1/audio/vad` on strixhalo, which is the same Silero with the operator's own tuning (§3b).
`%LOCALAPPDATA%\screenpipe\vad\silero_vad.onnx` exists if this is ever revisited; it would need
a reason connected to a user-visible problem, and a noisy room is the one case §4b does not cover.

Actually next:

1. **Use it for a while and watch three lines.** Everything below §3 changed today and none of it
   has had a real day's dictation: `window_slide` should have stopped (30 s window), 5-byte
   `"Yeah."` commits should have stopped (the VAD gate), and a session should now survive past two
   minutes of continuous talking (§8's timer fix).
1b. **Dictate into a document with `model_id: "windows-speech"`.** §3d is verified through the
   `Backend` interface against synthesized speech, which is not the same claim: a real microphone
   in a real room is exactly the case §4b says is not covered, and this model invents over
   non-speech more readily than Qwen does. Two specific things to watch — whether the ~3 s wait
   after the endpoint is tolerable in practice under end-of-session cadence, and whether anything
   Welsh, or otherwise unrelated to what was said, reaches the document.
2. **Record the Nemotron/zipformer quality finding in `bench/CANDIDATES.md` §4.** The picks table
   still recommends models the operator has measured as worse than Qwen. Now overdue on two
   counts: `publish_stable_partials=False` means commit-at-the-endpoint is the permanent latency
   profile if the quality winner is a batch model, and §3b's numbers (34–55× realtime) invalidate
   the latency arithmetic the table's argument rests on.
3. **Decide what the product does when the far end is unreachable** — half-answered, and the
   remaining half is now a product decision rather than an engineering one. `windows-speech` (§3d)
   is a complete local path, so the dependency is no longer forced: plan.md §2 can be honoured by
   choosing that entry. What does *not* exist is any automatic behaviour. Selecting a model is a
   `settings.json` edit and a restart, so a LAN outage mid-session still ends that session; nothing
   detects strixhalo being down and nothing falls back. Whether it should is the open question, and
   it is a real one — a silent fallback would swap a 550 ms model for a 3 s one without saying so,
   which is its own kind of wrong. Also still true: strixhalo serves *both* transcription and VAD,
   so the qwen entries have two dependencies on it, not one.
4. **Two clipboard paths have still never executed.** The injection policy for `notepad.exe`, where
   the open question is whether the clipboard is restored afterwards; and the new
   `set_clipboard_text` rescue for a deferred buffer whose target has gone (§3c), which is
   deliberately the one that does *not* restore.
5. **`settings.cpp` has no tests at all.** Noticed while adding `Cadence::EndOfSession`: the
   round-trip was verified by starting the app and watching it not complain. That is a real gap in
   a 1900-line file that parses and writes the user's configuration.

---

## 7. Conventions

* **Commit after each working step, directly to `main`.** Verify before committing.
* No third-party C++ dependencies. No Qt. Everything in-repo, including the JSON parser.
* No header under `src/` includes `<windows.h>`; handles cross module boundaries as `uintptr_t`.
* The sidecar core is stdlib-only. Only individual backends may import numpy/onnxruntime/torch,
  each inside its own module, so a missing runtime disables one backend rather than the server.
* `..\wan-dancer\.venv` is **read-only**. Run its Whisper baseline; install nothing into it.
* Comments explain *why*. Where something is unverified, the code says so rather than asserting.
* **Measurements and observations are different things**, and `docs/supported-targets.md` keeps
  them in different sections. Nothing not taken with the probe may fill in a cell.
* **The document invariant is not negotiable** (plan.md §1.4): committed text is never rewritten,
  never taken back, no backspaces. Everything may append. When a backend misbehaves the client
  reports it and preserves the document — it does not repair it.

---

## 8. The recurring failure mode, six sessions running

**A contract implemented on one side and never called from the other.** The clearest instance yet
is §3's, because the failure mode struck *twice over* on the same incident and the second strike
was the fix for the first:

* `session.cpp`'s non-fatal `error` branch discarded the child's whole diagnosis. The sidecar had
  built it, sent it over the wire, and the receiver dropped it at one line.
* So the previous session added a reader for `o.error` — and added it to a path this abort does
  not take. The fix for "a diagnosis nobody reads" was itself never reached. **That is what made
  §6 item 1 wrong**: it prescribed one dictation to produce a log line that could not appear.

The habit that would have caught it: the log said `31 byte reason`, and 31 is the length of the
fixed string on one specific branch. The evidence identifying the true path was already in the
file that was being read for evidence.

**2026-08-21, and the costliest yet because it was user-visible.** `LocalEvent::SpeechDetected` is
the only writer of `last_voice_ms`, and it was raised from **nowhere** in `src/` — all four call
sites were in `tests/`. Both of plan.md §1.6's silence timers hang off that value, so:

* the endpoint timer, also guarded by `utterance_open` (set only in that same unreachable handler),
  **never fired at all**;
* the session timer has no such guard, so it measured **wall-clock since activation** rather than
  silence — a hard **two-minute cap on every dictation**, however much the user was saying.

A green suite said otherwise the whole time, because the tests raised the event themselves. That is
the shape to watch for: a test that supplies the input the product never supplies is testing a
function the product does not compute. Fixed by deriving the event from the sidecar's own
`partial`/`final` text — the component that owns the VAD — rather than adding a second detector.

Earlier instances:

* `min_confidence`, `is_usable()` and `current_fallback()` all existed, were tested, and were
  called from nowhere in `src/` — the degradation rule was implemented *nowhere*.
* `note_session_ended()` was called from one of the two paths a session can end through, so an
  abort left the hotkey permanently dead.
* `o.error` — the session machine's full diagnosis — was built on every error and read by nothing.

And its measurement-side twin: **a harness that is not faithful to the product measures a different
function.** The Phase 0B row was measured three times before its numbers meant anything: first with
five of eleven scenarios arranged wrongly, then scoring against an incumbent anchor the product
always has and the probe never supplied. Both times the numbers were stable to the pixel and wrong.
Reproducibility says nothing about validity.

The habit that catches all of these: **before trusting that something works, check which entry
points the product actually calls.** A green test suite answers a different question.
