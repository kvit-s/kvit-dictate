# Handover — kvit-dictate

Written 2026-08-18, end of the fifth session. Commit `9ea874c` on `main`.

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
of 162 cells measured, all on `A09`, and they were measured twice — the first sweep's numbers are
superseded because five of its eleven cells did not arrange the scenario they were named for.
**The self-drawn-caret case is now the good news rather than the bad**: with no system caret at
all, source C landed within 0.12 line heights, three times over. The bad news moved to `S07`,
where a line wrap puts the true insertion point *outside the capture region* — a limitation of
`capture_region_for`, which the product uses by the same rule. What is still untrue of the
product: **no real speech has ever been transcribed** — every word so far came from the mock's
script — and no model has been downloaded. 806 tests pass across four suites at zero warnings.

---

## 2. How to build and test

```
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
build-windows-msvc-debug\bin\Debug\kd_tests.exe            # 305 tests / 35875 checks
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
| C++ modules | All 13 implemented. 305 tests. |
| `src/main.cpp` | Runs end to end against real targets. Still no test touches it. |
| Python sidecar | 372 tests. All five backends registered and reachable by name; only `mock` has run. |
| Bench harness | 129 tests. Never run against real data. |
| `tools/testtarget` | Now self-measuring: side channel wired to the probe, log pane suppressible. |
| `tools/probe` | **11 of 162 cells measured, twice** — the first set is superseded (§5.3). `--dump-state`/`--send` read the target without injecting; `--settle-ms` tests for mid-repaint diffs. |

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
its one hard rule (`sweep.ps1` takes over the foreground; do not run it while using the machine).
**`verify-arrange.ps1` is not that** — it injects nothing and never touches the foreground, so it
is safe at any time. Run it before every sweep.

### 5.1 The results, and the honest reading

`A09`, `sendinput`, REF-2, three repetitions each, every cell reproducing **to the pixel**, zero
records flagged `suspect`, all eleven arrangements verified before the sweep:

| Cell | error_lh | Cell | error_lh |
|---|---|---|---|
| S01 empty field | **0.07** | S08 horizontal scroll | **0.24** |
| S02 mid-line | 9.80 | S10 self-drawn caret | **0.12** |
| S03 selection | 11.67 | S11 dark / S12 light | 9.80 each |
| S04 line wrap (see below) | **0.07** | S16 LTR into RTL control | **0.07** |
| S07 wrap at the viewport bottom | **48.07** | S17 password | **refused, correctly** |

**Read those as source C's error, not the product's.** The probe scores `anchor_from_diff`
unconditionally; the product ranks a real caret (0.95) above any diff (≤0.90), and `source_a` was
available in every cell except `S10`. The shipping product would have used the caret and been
exact in all of them.

**`S10` is answered, and the answer is good.** It is the only row with no system caret, so source C
is the only anchor — the VS Code / Slack / terminal situation. **0.12 line heights, about 2 px,
three times over.** It is `S01`'s arrangement with exactly one variable changed, so the gap between
0.07 and 0.12 is the absence of source A and nothing else; and with no caret to centre on, the
capture region fell back to the whole window, which is the same bootstrap path
`App::submit_before_frame` takes. Caveat: it is a clean append into an empty field. `S02`'s reflow
error and `S07`'s below apply to a self-drawn target too, and neither has been measured there.

**`S07` is where the real problem moved, and it is the product's, not the estimator's.** The wrap
moves the insertion point 728 px leftward; `capture_region_for` (900×220 at 96 dpi, insertion point
at 65 % of the width) reaches only 585 px behind the previous one, so the truth was 143 px outside
the captured rectangle and nothing inside it could have found it. `looks_like_scroll` stayed false
for a second geometric reason: the short numbered lines end left of the region, so within it the
scroll moved blank pixels. `--settle-ms 600` reproduces 48.07 exactly, so it is not a partial
repaint. `src/main.cpp:841` picks the region by the same rule. **The threshold is concrete: an
insertion point more than 585 px into a line at 96 dpi can wrap outside the capture region.**

`S02`/`S03` fail exactly as §2 of the supported-targets doc predicted: text to the right of an
interior insertion reflows, so the rightmost changed pixel is the end of the line. `S11` and `S12`
are identical to the pixel: theme does not affect the estimate.

**Two of the good numbers are weaker than they look.** `S04`'s long line *already wraps* when the
scenario is arranged, so the injection appends to the last visual line and never causes a wrap —
0.07 measures an append inside a wrapped paragraph. And `S08`/`S16` both have
`caret_before_injection == ground_truth` (the `ES_AUTOHSCROLL` caret stays pinned at the right edge
while text slides left; the right-aligned RTL control's append point does not move), so an
estimator that just returned the previous anchor would score the same.

### 5.2 Known defects in the harness itself

* **`winner` does not follow the product's rule.** The probe prefers source C whenever the diff is
  non-empty, so the field does not answer §3.3 ("anchor source that won"). Recorded, not fixed.
  This is now the main thing stopping the §5 matrices in the supported-targets doc from being
  filled in at all.
* **`S05`/`S06` need a third injection** (newline-only, whitespace-only). `--text` has no value
  for them.
* **`S09` cannot be arranged** — `tools/testtarget` cannot raise an autocomplete popup.
* **`S04` does not construct the wrap it is named for** (see above). Fixing it means padding the
  long line to the control's width the way `TextManyLines` now does for `S07`.

### 5.3 The first sweep of the day is superseded — do not cite its numbers

Five of its eleven cells did not arrange the scenario they were named for, and
docs/supported-targets.md §5.9.3 records why in full. The short version, because the shape is the
one this project keeps hitting: **`EM_SETSEL(-1, -1)` does not move an edit control's caret to the
end** — `wParam = -1` means "deselect" — so `S04`, `S07`, `S08` and `S16` all injected at offset 0.
`S07` and `S08` therefore never scrolled at all, which read in the records as a scroll-detection
failure and was not one. Separately, `S07` switched on the pane's "scroll on each insertion" mode
and nothing switched it off, so `S10` ran with it live and walked itself off the screen.

The guard against a repeat is `verify-arrange.ps1`, which checks each cell's arranged state —
focus, caret offset *in the text*, text, theme, system-caret presence, leaked scroll mode — against
a declared `expect` block in `tools/phase0b/arrange.ps1`. `arrange.ps1` is dot-sourced by the
verifier, the sweep and `runcell.ps1` alike, so what is verified is what is measured.

---

## 6. Defects found this session, and the pattern that produced them

Every one is the same shape as the nineteen before it: **a contract implemented on one side and
never called from the other.** It is now five sessions in a row.

### 6.0 The fifth session's, all in the measurement harness

The sweep *declared* what each cell arranged and nothing ever asked the target whether that was
what it had — the same shape, one level up: a contract asserted on one side and never checked from
the other. Full detail in §5.3 and docs/supported-targets.md §5.9.3. Worth carrying forward:

* **A measurement harness is code, and it needs its own oracle.** Eleven cells, three repetitions
  each, zero suspect records, numbers stable to the pixel — and five of them were measuring
  something other than what they were labelled. Reproducibility said nothing about validity;
  the arrangements were reproducibly wrong.
* **Two of the wrong readings pointed at the product.** "Scroll detection did not fire" was
  "nothing scrolled", and "the estimator is hopeless without a system caret" was "the target
  scrolled itself off the screen". A bad harness does not just lose information, it manufactures
  plausible defects in code that is fine.
* **`--dump-state` earns its keep.** Being able to ask the target what state it is in, without
  injecting or capturing anything, turned a foreground-stealing guess-and-check loop into a check
  that runs in two seconds while the machine is in use.

### 6.1 The fourth session's

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

1. **`min_confidence` is dead configuration — nothing in `src/` reads it.** Found by replaying
   the `S07` measurement through the product's own functions
   (`tests/test_anchor.cpp`, `anchor.s07_*`, both passing). The chain:

   * `anchor_from_diff` rejects only `confidence <= 0`, so `S07`'s 0.4953 survives.
   * `should_replace_anchor` short-circuits on `candidate.source == current->source` with
     "this is a refresh, not a switch" and returns **true** without consulting
     `cfg.min_confidence`. On a target with no system caret, source C is always the incumbent, so
     every subsequent source C answer is accepted at any confidence above zero.
   * `Anchor::is_usable(threshold)` — the gate plan.md §1.5 describes — is referenced **only by
     `tests/test_anchor.cpp`**. `AnchorEstimator::current_fallback()` is likewise called from
     nowhere in `src/`; `overlay.cpp:569` has a comment saying `main.cpp` "is expected to call
     `set_mode()` accordingly", and `main.cpp` does not.
   * `Overlay::set_anchor` stores whatever it is handed.

   So `settings.anchor.min_confidence` and `below_threshold` parse, validate, round-trip and do
   nothing. This is the twentieth instance of the pattern in §6, and the most consequential:
   plan.md §1.5's deterministic-degradation rule is unimplemented on the path `main.cpp` drives.

2. **Then `S07`'s capture region.** A wrap can move the insertion point outside the captured
   rectangle, and `src/main.cpp:841` chooses that rectangle the same way the probe does. Note the
   failure mode is **not** a jump: the estimate lands at (1150, 445), 7 px from where the caret was
   *before* the injection, so the overlay sits still while the text walks away from it. Options,
   once the gate above exists: widen the region (costs capture time on every commit), re-centre it
   (the 65/35 rightward bias assumes text grows rightward, which is backwards when a wrap fires),
   or accept that 0.4953 is the estimator correctly reporting "I do not know" and let the gate hold
   the previous anchor.
3. **The two checks in §4** — one session each, both need a human.
4. **Fix `S04` to construct a real wrap**, then measure `S02`'s reflow case and `S07`'s wrap case
   *without* a system caret. `S10` says source C handles a clean append into a self-drawn target;
   nothing says it handles the two cases that are known to be hard there. That is a small change
   to `arrange.ps1` plus one sweep.
5. **The rest of the `A09` row**, then a second application. `A09` is the calibration row; a real
   application's numbers are only interpretable against it, and it is not finished. Making the
   probe's `winner` follow the product's rule is the prerequisite for filling the §5 matrices,
   whose `Src` column it currently answers wrongly.
6. **Fetch the ONNX model** (`bench/CANDIDATES.md` pick 1, a few hundred MB, no PyTorch) and make
   the sidecar real. What remains is `envs/onnx`, the weights, and the first real `load()`. Expect
   `sherpa_onnx_streaming.py`'s eight recorded UNVERIFIED assumptions to be what that run is about.
   `UV_CACHE_DIR` must be on `D:` — uv hardlinks from cache into venvs and hardlinks cannot cross
   volumes.
7. **Record the corpus** with `bench/record.py` — read its report first; it is written against the
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
  them in different sections. Nothing that was not taken with the probe may fill in a cell. §5.9.1,
  §5.9.2 and §5.9.3 are worth reading before adding any: they record why REF-1 cannot measure
  anchor error, what the A09 cells do and do not say, and how five of them came to be measured
  under the wrong names.
* **Never run a sweep without `verify-arrange.ps1` passing first.** A measurement of an
  unverified arrangement is not a weak measurement, it is a measurement of an unknown scenario —
  and it will be stable to the pixel while being wrong, which is what makes it dangerous.
* **Superseded measurements stay on disk.** They are accurate about what was on screen; it is the
  label that was wrong. Deleting them would hide why the published numbers changed.
* Empty test suites are registered `DISABLED` in `CMakeLists.txt` rather than omitted.
