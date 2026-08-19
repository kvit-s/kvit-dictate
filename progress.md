# progress — kvit-dictate

Written 2026-08-18. Commit `eef533e` on `main`.

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

**It is not yet reliable.** A long sentence aborts partway with a protocol-level stability
violation and the tail of the sentence is lost. See §3.

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
build-windows-msvc-debug\bin\Debug\kd_tests.exe        # 326 tests / 35920 checks
cd build-windows-msvc-debug && ctest -C Debug          # 15 suites, none disabled

python -m unittest discover -s server/tests -t server  # 372 tests   (from the repo root)
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

Model entries that matter:

| id | what it is |
|---|---|
| `qwen-asr-http` | LAN Qwen3-ASR, re-transcribes once a second → live preview that revises. **Trips the stability abort (§3).** |
| `qwen-asr-http-finalize` | one request per utterance, no live preview. Cannot trip it. The reliable one. |
| `mock` | scripted; the only entry that needs nothing at all. Still the way to test the C++ side in isolation. |
| `whisper-large-v3-turbo` | zero-install baseline: the weights (1.6 GB) and the venv both already exist on this machine. Never run. |

---

## 3. The open bug, and it is the one that matters

**A long sentence aborts mid-way and loses its tail.** Reproduced twice. The session ends, the
overlay flags a violation, and everything not yet committed is discarded.

What is known:

* It is a **protocol-level §8.2 stability violation** — the sidecar's own `stable_bytes` went
  backwards or its stable prefix mutated — raised in `session.cpp`, **not** the commit engine's
  `CommittedTextContradicted` check. `grep -c "committed-prefix violation:"` over the whole log is
  **zero**: that check has never fired.
* The cause is structural. A batch model re-transcribing the whole window every second
  re-punctuates and re-cases earlier words once it sees where the sentence ended — `how are you`
  becomes `How are you?` — and that revises text below the published stable mark.
* `qwen-asr-http-finalize` cannot hit it: one transcription per utterance, nothing to contradict.

What was done about it today:

* `main.cpp` now logs `o.error` — code, message and byte offset. The session machine had always
  built a precise diagnosis and nothing read it, which is why this was misdiagnosed once already.
  **The next occurrence will name the exact rule and offset.** Get that line before designing a fix.
* `CommitEngine::Result::salvage` appends the words a rejected hypothesis still agrees the position
  of, instead of dropping the rest of the sentence. Correct, tested (7 cases), and **not** on this
  path — it is in the commit-engine branch that never runs. It will matter once this one is fixed.

The likely shape of the fix, in the order to try it:

1. Read the new `session error:` line. Which of the three §8.2 rules broke, and at what offset?
2. If the sidecar is publishing a `stable_bytes` its own next hypothesis contradicts, that is the
   Python side over-claiming stability. `SessionConfig.stability_n/m/hold_ms` (2 / 3 / 200 ms) are
   tuned for a natively-streaming model, not for one that revises punctuation at the end.
3. Only then consider whether the client should tolerate it rather than abort.

---

## 4. Also true, and smaller

* **The VAD is the weak fallback.** Silero is not configured, so the stdlib energy/ZCR endpointer
  is in use — its own docstring says it "detects sound rather than speech and is measurably
  worse". It triggers on keyboard noise. Fix: point `VadConfig.silero_model_path` at a local
  `silero_vad.onnx`; plan.md §1.9 deliberately refuses to download it.
* **The model hallucinates over silence**, measured: 500 ms of digital silence returns `"Yeah."`,
  2000 ms returns `"Okay."` No guard is implemented on purpose — the endpoint returns a bare
  string with no timestamps to filter on, and a phrase blocklist would silently delete words a
  user really said. VAD gating upstream is the real fix, which makes the weak VAD above matter
  more than it looks.
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

1. **Reproduce the §3 abort and read the new `session error:` line.** One dictation, one log line,
   and the fix stops being guesswork. Everything else in this list is smaller.
2. **Tune the sidecar's stability for a revising backend** (`--stability-n`, `--stability-m`,
   `--stability-hold-ms`), or make the Python side stop publishing a `stable_bytes` its own next
   hypothesis will contradict.
3. **Point the VAD at a local `silero_vad.onnx`.** Cheap, and it improves both endpointing and the
   silence-hallucination exposure.
4. **Record the Nemotron/zipformer quality finding in `bench/CANDIDATES.md` §4**, with whatever
   numbers exist. The picks table currently recommends models the operator has rejected.
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

**A contract implemented on one side and never called from the other.** Today's instances:

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
