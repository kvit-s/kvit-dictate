# progress — kvit-dictate

Written 2026-08-21. Commit `a70b14d` on `main`.

Read with `plan.md` (the specification), `bench/CANDIDATES.md` (model picks) and
`docs/supported-targets.md` (measurements + open questions §9). This file is only: what works,
what does not, and what to do next.

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
build-windows-msvc-debug\bin\Debug\kd_tests.exe        # 328 tests / 35956 checks
cd build-windows-msvc-debug && ctest -C Debug          # 15 suites, none disabled

python -m unittest discover -s server/tests -t server  # 392 tests   (from the repo root)
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
   is `--endpoint-silence-ms` in **`models.json`**, now **1500 ms** for both qwen entries (was 800,
   which split a sentence at every breath). The client-side value is kept at 1500 only so the two
   do not read as disagreeing.
2. **How often text is typed** — `settings.json`'s `cadence.mode`, now **`end-of-session`**. See
   §3c. The two are independent: the sidecar still decodes about a sentence at a time and still
   uploads at most an 8 s window, whatever the client does with the results.

Model entries that matter:

| id | what it is |
|---|---|
| `qwen-asr-http` | LAN Qwen3-ASR, re-transcribes once a second → live preview that revises. Used to trip the stability abort; fixed in `89d012e` and confirmed working (§3). Commits once per utterance, at the endpoint. |
| `qwen-asr-http-finalize` | one request per utterance, no live preview. Cannot trip it. The reliable one. |
| `mock` | scripted; the only entry that needs nothing at all. Still the way to test the C++ side in isolation. |
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
* **This is the silero question, answered without a local install.** §4's open item was whether to
  put `onnxruntime` on this machine; the server now does it, with the operator's own tuning.

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

## 4. Also true, and smaller

* **The local VAD is still the energy/ZCR fallback, and it matters less than it did.** Silero is
  not configured locally, so the stdlib endpointer does the *endpointing* — deciding when an
  utterance ends. What it no longer does is decide what reaches the model: the server's VAD route
  now gates that (§3b), which was the half that could put invented words in the document.
  * This file used to assert "it triggers on keyboard noise" as fact. **Unverified**, and an
    attempt to reproduce it (2026-08-21) failed — digital silence and synthetic key-click
    transients were all rejected, `gate_open=False`. That test was synthetic and effectively a
    quiet room, so it does not clear the detector either; it means nobody has caught it
    misbehaving.
  * A local silero would still be better at *endpointing* (energy/ZCR measures loudness, silero
    measures speechiness, and loudness degrades with a fan, music or a nearby conversation). But
    the argument for it is now latency and boundary precision, not correctness. `onnxruntime` is
    not installed in `C:\Python312`; `%LOCALAPPDATA%\screenpipead\silero_vad.onnx` exists if
    it is ever wanted.

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

1. ~~Dictate the sentence that used to abort.~~ **Done, 2026-08-18 22:09.** Clean: no violation,
   no `session error:` line, seven utterances committed over 47 seconds. §3 is confirmed.
2. ~~Tune the sidecar's stability for a revising backend.~~ **Ruled out** — §3 says why the three
   knobs cannot fix a revision at byte 0. Done instead by pinning the mark to 0 for the whole
   class of backend.
3. **Point the VAD at a local `silero_vad.onnx`.** Cheap, and it improves both endpointing and the
   silence-hallucination exposure. **Now the largest known defect** — and §3's fix made it *more*
   load-bearing, not less: with nothing committed until the endpoint, the endpointer is what
   decides when the user's words reach the document. A false endpoint on keyboard noise now splits
   a sentence into two commits; a missed one holds the whole sentence back.
4. **Record the Nemotron/zipformer quality finding in `bench/CANDIDATES.md` §4**, with whatever
   numbers exist. The picks table currently recommends models the operator has rejected. §3 raises
   the stakes on this: if the quality winner really is a *batch* model, then
   `publish_stable_partials=False` — commit at the endpoint, never mid-sentence — is the product's
   permanent latency profile rather than a workaround, and that belongs in the picks table where
   the decision gets made.
5. **Decide what the product does when the far end is unreachable.** This backend has no local
   fallback, and plan.md §2's "no network access during dictation" is broken in the letter by it —
   see the module docstring for the full argument. If the LAN endpoint is the long-term plan rather
   than a bring-up step, that guarantee should be rewritten rather than quietly violated.
6. **The clipboard injection path has still never executed.** Selected by policy for `notepad.exe`,
   and the question is whether the clipboard is restored afterwards.

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

## 8. The recurring failure mode, five sessions running

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
