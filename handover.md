# Handover — kvit-dictate

Written 2026-08-18, end of the fourth session. Commit `def679f` on `main`.

Read this with `plan.md` (the specification), `README.md` (current status) and
`docs/protocol.md` (the wire contract). This file covers only what those three do not: where the
work actually stands, what is untrue about it, and what to do next.

---

## 1. State in one paragraph

**Dictation works, and a human has now watched it work end to end.** Press `Win+Y` in Notepad3,
speak, pause, speak again: the first utterance is punctuated and committed, a `final` closes it
without ending the session, the second utterance is appended after a space, and an inline overlay
sits at the caret and follows it. Four of the five checks the last handover left open are
confirmed; the fifth (clipboard into Windows 11 Notepad) is untried. **Phase 0B has started**: 11
of 162 cells measured, all on `A09`, and the harness that produced them was rebuilt three times
before its numbers meant anything. What is still untrue of the product: **no real speech has ever
been transcribed** — every word so far came from the mock's script — no model has been downloaded,
and the anchor's behaviour on targets that draw their own caret (VS Code, Slack, terminals) is
measured as *bad* and not yet understood. 804 tests pass across four suites at zero warnings.

---

## 2. How to build and test

```
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
build-windows-msvc-debug\bin\Debug\kd_tests.exe            # 303 tests / 35863 checks
cd build-windows-msvc-debug && ctest -C Debug              # 14 suites, none disabled

python -m unittest discover -s server/tests -t server      # 372 tests  (run from repo root)
python -m unittest discover -s bench/tests  -t .           # 129 tests  (run from repo root)
```

Use `C:\Python312\python.exe`. The `python` on PATH in Git Bash is a conda 3.9 and will fail.

Working notes that cost time to rediscover:

* **Kill any running `kvit-dictate.exe` before rebuilding.** It holds the link output *and* the
  single-instance mutex, so a stale one produces `LNK1168` *and* a new instance that exits
  immediately — two confusing symptoms of one cause. `taskkill /F /IM kvit-dictate.exe`.
* `cmake` is at `C:\Program Files\CMake\bin\cmake.exe`; Ninja is NOT on PATH, so the VS 2022
  generator is the only one configured.
* The two Python suites must be run **from the repository root**; `-t` sets the import root.
* `kd_tests.exe --list` prints every test; `--suite <name>` filters.
* `KD_WERROR` is **ON**. Debug and Release both build at zero warnings.
* **A second launch of `kvit-dictate.exe` opens the settings dialog** on the running instance and
  exits. That is the single-instance path, and it is a convenient way to reach the dialog without
  the tray icon.

### 2.1 Running it, and reading what happened

`%LOCALAPPDATA%\kvit-dictate\settings.json` currently has `model_id: "mock"`, `log_level: debug`
and `diagnostics_file_enabled: true`. The log lands in
`%LOCALAPPDATA%\kvit-dictate\logs\kvit-dictate.log` and is the first thing to read after any
session — it answers "which model was actually loaded" in one line, which cost a wasted session
last time.

Four model entries matter:

* `mock` — the VAD is in the path, so it needs real speech. **This is the one that produces
  utterance boundaries**, and therefore the only one that can exercise anything to do with more
  than one utterance.
* `mock-ungated` — `--vad none` plus `silence_peak_threshold=0`. Advances on any audio including
  silence, so it needs no speech. **But `--vad none` means one utterance per session, and the mock
  resets its script index at every session start, so this entry can only ever produce script line
  1.** Do not use it to test anything about utterance boundaries; that is what cost a session.
* `mock-hostile` — `mock-ungated` plus a sidecar that knowingly violates the protocol: after two
  clean emissions the reported `stable_bytes` goes backwards. **Never run against the C++ side.**
  See §5.
* `whisper-large-v3-turbo`, the three ONNX rows — registered and reachable, never loaded.

**Nothing can test the hotkey chord except a human.** The input filter ignores every event flagged
`LLKHF_INJECTED` by design (plan.md §1.8), so a synthesised `Win+Y` activates nothing, ever.

---

## 3. What is done

| Area | State |
|---|---|
| `docs/protocol.md` + 20 fixtures | Written; both endpoints execute the same corpus. |
| C++ modules | All 13 implemented. 303 tests. |
| `src/main.cpp` | Runs end to end against real targets. Still no test touches it. |
| Python sidecar | 372 tests. All five backends registered and reachable by name; only `mock` has run. |
| Bench harness | 129 tests. Never run against real data. |
| `tools/testtarget` | Now self-measuring: side channel wired to the probe, log pane suppressible. |
| `tools/probe` | **11 of 162 cells measured.** `tools/phase0b/` automates the `A09` row. |

### Explicitly NOT done

* **No model downloaded, no environments created.** `envs/` does not exist. plan.md §1.9 makes
  fetching a deliberate user-initiated act, so this is correct rather than an oversight — but
  every backend except `mock` is untested against reality.
* **No corpus recorded.** `bench/prompts/*.jsonl` hold 91 real prompts; no audio exists.
* **The clipboard injection path has never executed.** Selected by policy for `notepad.exe`.
* **`mock-hostile` has never run against the C++ side.**
* **151 of 162 Phase 0B cells unmeasured**, including every row except `A09`.

---

## 4. The one remaining check from the original five

Everything else in the old §4 is confirmed working (§7). This is what is left:

**Clipboard into Windows 11 Notepad.** `notepad.exe` has an app policy selecting the clipboard
method, because Win11's Notepad garbles paced `SendInput` (§5.10 of the supported-targets doc).
Two questions: is the pasted text clean, and **is the clipboard restored afterwards**? Copy the
word `CANARY` first, dictate, then `Ctrl+V` somewhere and see whether `CANARY` comes back.
plan.md §1.7 restores only if we still own what we set — losing a user's clipboard is worse than
leaving ours in it.

And one that was added this session:

**`mock-hostile`, into `kd_testtarget.exe` and nothing else.** The sidecar deliberately retracts
text it has already told the client to type. Does the commit engine refuse the retraction and log
it, or does the document get corrupted? docs/protocol.md §8.3's handling is covered by fixtures at
the codec level and has never run against a live sidecar. **Do not point this at a real document.**

---

## 5. Where Phase 0B actually stands

`kd_probe.exe --status` prints the matrix. `tools/phase0b/README.md` explains the automation and
its one hard rule (it takes over the foreground; do not run it while using the machine).

### 5.1 The results, and the honest reading

`A09`, `sendinput`, REF-2, three repetitions each, zero records flagged `suspect`:

| Cell | error_lh | Cell | error_lh |
|---|---|---|---|
| S01 empty field | **0.07** | S08 horizontal scroll | 17.40 |
| S02 mid-line | 9.33 | S10 self-drawn caret | 31.53, then no estimate |
| S03 selection | 11.67 | S11 dark / S12 light | 9.33 each |
| S04 line wrap | 9.33, 9.33, 49.95 | S16 RTL control | **0.13–0.24** |
| S07 vertical scroll | 14.93 | S17 password | **refused, correctly** |

**Read those as source C's error, not the product's.** The probe scores `anchor_from_diff`
unconditionally; the product ranks a real caret (0.95) above any diff (≤0.90), and `source_a` was
available in every cell except `S10`. The shipping product would have used the caret and been
exact in all of them.

**Which makes `S10` the cell that matters, and the next thing to work on.** It is the only row with
no system caret, so source C is the only anchor — the VS Code / Slack / terminal situation, i.e.
every target anyone cares about. It produced 31.53 line heights once and *no usable estimate at
all* twice, and its ground truth came back at `y = -226`, off-screen, which means the scenario
setup is wrong as well. Both have to be untangled before the number means anything.

`S02`/`S03` fail exactly as §2 of the supported-targets doc predicted when the scenarios were
written: text to the right of an interior insertion reflows, so the rightmost changed pixel is the
end of the line, not the insertion point. `S07`/`S08` fail with `looks_like_scroll` **false**, so
scroll detection did not fire — worth a look, it is supposed to score those ~0 and hold the
previous anchor. `S11` and `S12` are identical to the pixel: theme does not affect the estimate.
`S16` passing is not a win — REF-2 is LTR-only, so it measures an LTR injection into an RTL
*control*, not RTL text.

### 5.2 Known defects in the harness itself

* **`winner` does not follow the product's rule.** The probe prefers source C whenever the diff is
  non-empty, so the field does not answer §3.3 ("anchor source that won"). Recorded, not fixed.
* **`S05`/`S06` need a third injection** (newline-only, whitespace-only). `--text` has no value
  for them.
* **`S09` cannot be arranged** — `tools/testtarget` cannot raise an autocomplete popup.
* **`S04`'s third repetition drifted** (49.95 against 9.33 twice), so its state is not being fully
  re-armed. The other cells reproduce to the pixel, so it is specific to that setup.

---

## 6. Defects found this session, and the pattern that produced them

Every one is the same shape as the nineteen before it: **a contract implemented on one side and
never called from the other.** It is now four sessions in a row.

1. **The four real backends were unreachable.** ~3800 lines of finished adapter; the registry
   listed only the mock. `open_backend` documented a `from_backend_config` hook nothing provided,
   so `--model`/`--chunk-ms`/`--device` were discarded silently. `SherpaOnnxStreamingBackend` was
   not a `Backend` at all — `backends/sherpa_adapter.py` is the translation it never had.
2. **`stable_bytes_override` was manufactured and dropped one function later.** The seam now
   carries it, gated on `--allow-hostile-backend`.
3. **The settings dialog rendered a blank page on every tab switch.** Overlapping sibling pages,
   shown before the outgoing page was hidden, with no `WS_CLIPSIBLINGS` and no forced repaint.
   Every control existed, sat at the right coordinates and reported `IsWindowVisible() == true`
   the whole time.
4. **`Effect::ResetCommitEngine` fired at utterance boundaries.** It is session-scoped, so it
   cleared `session_has_committed` — the only input to `CommitOp::leading_space`. The event that
   must produce the space between utterances was the event that destroyed the means of producing
   it, which is why `…the default modelopen…` came back after being fixed once.
5. **The probe never called `tools/testtarget`'s side channel**, captured the whole window instead
   of the product's region, read ground truth before the injection, raced the injection, and was
   contaminated by the target's own log pane repainting on every keystroke.

**The lesson that generalises, from defect 4:** its earlier fix had a passing test that drove
`CommitEngine::begin_utterance()` — and *nothing in the product calls `begin_utterance()`*.
"Covered by a test" and "covered on the path `main.cpp` drives" are different claims. When
something does not work, check which entry points the product actually uses before trusting a
green suite.

### The one that was not a code defect

**An automated sweep typed REF-1 into the operator's editor.** The launching script checked the
foreground window, and that check was worthless: it ran before a one-second countdown inside a
separate process. `kd_probe --require-exe NAME.exe` now checks twice inside the probe, at target
capture and immediately before the events go out, and refuses rather than warns. Anything that
drives the probe unattended must pass it.

---

## 7. What a human confirmed, so nobody re-tests it

* **The overlay paints, inline, at the caret, and follows it** as text commits. This is plan.md
  §1.5 working as specified rather than the floating-pill fallback — against Notepad3.
* **The endpointer closes an utterance without ending the session.** Speak, pause >1 s, speak: a
  second script line appears, which is only possible if a `final` closed the first utterance.
* **The space between utterances is back** (after defect 4 above).
* **The finalising chord is read correctly** — the session ends `hotkey_pressed`, not
  `stop_on_input`.
* **Four targets characterised** (observations, not measurements — §5.10 of the supported-targets
  doc): Notepad3 inline and clean; **Win11 Notepad garbles paced SendInput**; VS Code's integrated
  terminal clean (`console=0`, correctly — xterm.js is not a console); **Windows Terminal**
  `console=1` detected, text lands cleanly, overlay degrades to the pill and does not move.
* **`Win+H` — Windows' own dictation — previews in VS Code's terminal and not in Windows
  Terminal.** Microsoft draws the same line we do, in the same place. That is the argument for
  capping `A03` at *Degraded* rather than spending the Windows.Graphics.Capture work on inline
  preview inside consoles.

---

## 8. Suggested order

1. **`S10`, and what it means.** The only measured cell where source C is the sole anchor is also
   the one that failed and whose setup is wrong. Until that is understood, nothing is known about
   the targets the product is for. Start by fixing the scenario (`y = -226` is not a caret), then
   decide whether 31 line heights is the estimator or the harness.
2. **The two checks in §4** — one session each, both need a human.
3. **The rest of the `A09` row**, then a second application. `A09` is the calibration row; a real
   application's numbers are only interpretable against it, and it is not finished.
4. **Fetch the ONNX model** (`bench/CANDIDATES.md` pick 1, a few hundred MB, no PyTorch) and make
   the sidecar real. What remains is `envs/onnx`, the weights, and the first real `load()`. Expect
   `sherpa_onnx_streaming.py`'s eight recorded UNVERIFIED assumptions to be what that run is about.
   `UV_CACHE_DIR` must be on `D:` — uv hardlinks from cache into venvs and hardlinks cannot cross
   volumes.
5. **Record the corpus** with `bench/record.py` — read its report first; it is written against the
   C++ capture path and may need an entry point `audio_capture` does not expose.

Untouched and still open: **pre-roll dropped across the VAD seam** (§6 of the previous handover —
`VadEndpointer` replays `preroll_ms` before an onset, `Session` never consumes it, the cost is the
leading fraction of the first word). It belongs in the same pass as measuring whether it matters.

---

## 9. Conventions to preserve

* **Commit after each working step, directly to `main`.** Verify before committing.
* No third-party C++ dependencies. No Qt. Everything in-repo, including the JSON parser.
* No header under `src/` includes `<windows.h>`; handles cross module boundaries as `uintptr_t`.
* The sidecar core is stdlib-only. Only individual backends may import numpy/onnxruntime/torch,
  each inside its own module, so a missing runtime disables one backend rather than the server.
* `..\wan-dancer\.venv` is **read-only**. Run its Whisper baseline; install nothing into it.
* Comments explain *why*. Where something is unverified, the code says so rather than asserting.
* **Measurements and observations are different things**, and `docs/supported-targets.md` keeps
  them in different sections. Nothing that was not taken with the probe may fill in a cell. §5.9.1
  and §5.9.2 are worth reading before adding any: they record why REF-1 cannot measure anchor
  error, and what the first eleven cells do and do not say.
* Empty test suites are registered `DISABLED` in `CMakeLists.txt` rather than omitted.
