# Handover — kvit-dictate

Written 2026-08-16, end of the implementation session. Commit `586ffaf` on `main`.

Read this with `plan.md` (the specification), `README.md` (current status) and
`docs/protocol.md` (the wire contract). This file covers only what those three do not: where the
work actually stands, what is untrue about it, and what to do next.

---

## 1. State in one paragraph

Both processes are implemented and wired end to end. The C++ client builds to a real 2.4 MB
`kvit-dictate.exe` with a tray icon, global hotkey, WASAPI capture, a secured named pipe, the
commit engine, the anchor estimator, both injection paths and a layered overlay. The Python
sidecar runs on stock CPython 3.12 with nothing installed and speaks the same protocol. 691 tests
pass across three suites, at zero warnings. **Nothing has ever transcribed real speech, no model
has been downloaded, no Phase 0B cell has been measured, and `main.cpp`'s wiring has never
executed outside a debugger.** The gap between "implemented" and "works" is entirely in the
things that need a human at the machine or a model on disk.

---

## 2. How to build and test

```
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
build-windows-msvc-debug\bin\Debug\kd_tests.exe            # 296 tests / 35807 checks
cd build-windows-msvc-debug && ctest -C Debug              # 14 suites, none disabled

python -m unittest discover -s server/tests -t server      # 266 tests  (run from repo root)
python -m unittest discover -s bench/tests  -t .           # 129 tests  (run from repo root)
```

Use `C:\Python312\python.exe`. The `python` on PATH in Git Bash is a conda 3.9 and will fail.

Working notes that cost time to rediscover:

* `cmake` is at `C:\Program Files\CMake\bin\cmake.exe`; Ninja is NOT on PATH, so the VS 2022
  generator is the only one configured.
* The two Python suites must be run **from the repository root**; `-t` sets the import root and
  running from inside `server/` or `bench/` fails with "Start directory is not importable".
* `kd_tests.exe --list` prints every test; `--suite <name>` filters.

---

## 3. What is done, and what that means

| Area | State |
|---|---|
| `docs/protocol.md` + 20 fixtures | Written, and **both endpoints execute the same corpus**. This is the only cross-language check that exists and it has already earned its keep (see §5). |
| C++ modules | All 13 implemented. 296 tests. |
| `src/main.cpp` | Written and compiles — the orchestrator that turns 12 modules into an app. **Never run.** No test touches it. |
| Python sidecar | Implemented, stdlib-only core, five backends behind lazy imports. 266 tests. |
| Bench harness | Recorder, metrics (incl. ICER), matrix runner with the pre-registered selection rule. 129 tests. |
| `tools/testtarget` | Implemented — the controlled app with edit/richedit/password/multiline controls that logs the code points it receives. |
| `tools/probe` | Implemented, builds, walks a resumable 162-cell matrix. **0 cells measured.** |

### Explicitly NOT done

* **No model downloaded.** `bench/CANDIDATES.md` picked the shortlist; nothing was fetched.
  plan.md §1.9 makes model fetching a deliberate, user-initiated act, so this is correct, not an
  oversight — but it means every backend except `mock` is untested against reality.
* **No environments created.** `envs/` does not exist. `server/requirements/*.txt` are pinned and
  `server/README.md` documents the exact commands, but nothing was installed.
* **No corpus recorded.** `bench/prompts/{tech,hard,ambient}.jsonl` contain 91 real prompts with
  the dev/heldout split baked into the data; no audio exists.
* **Phase 0B not run** — the critical gate. `docs/supported-targets.md` is all `—`.
* **No settings dialog.** The tray's "Settings…" opens `settings.json` in the shell instead.
  `settings.cpp` has `show_settings_dialog`, but `main.cpp` does not call it.

---

## 4. Do this first: the end-to-end smoke test

**`main.cpp` is the largest piece of never-executed code in the repository.** Every module below
it is unit-tested; the wiring between them is not. Find out whether the twelve modules actually
talk to each other before doing anything else.

`models.json`'s `mock` entry currently points its interpreter at
`D:\projects\kvit-dictate\envs\mock\Scripts\python.exe`, **which does not exist**. The mock
backend is stdlib-only, so the cheapest fix is to point it at the system interpreter:

```jsonc
"interpreter": "C:\\Python312\\python.exe",
"args": ["--backend", "mock"]
```

`IpcClient` appends `--pipe <name> --proto 1` itself. The child is launched with `module`
`kvit_asr`, so its working directory (or `PYTHONPATH`) must contain `server/` — set
`working_directory` on the `ChildSpec` or add `PYTHONPATH` to the entry's `env`. Verify the CLI
first, standalone:

```
cd server && C:\Python312\python.exe -m kvit_asr --help
```

That already works. Then run `kvit-dictate.exe`, press `Win+Y` with Notepad focused, and watch
for: tray reaching "listening", the sidecar spawning, `ready` arriving, mock partials committing
text into Notepad, and a second `Win+Y` finalising.

**Be at the machine for this.** It installs a global low-level keyboard hook and will synthesise
keystrokes into whatever window has focus.

Things most likely to be wrong on first run, in order:

1. The child's working directory / `PYTHONPATH`, so `-m kvit_asr` fails to import.
2. The `Effect` dispatcher in `App::apply` — the effect ordering is implemented from
   `session.h`'s contract and has never been observed.
3. `Effect::ApplyText` carries no payload; `main.cpp` passes the in-flight message alongside the
   `Outcome`. If a code path reaches `ApplyText` without a message, text is silently dropped.
4. Timer cadence: `on_tick` runs on a 50 ms `WM_TIMER`, which drives both silence timers and the
   commit cadence. Nothing has validated that interval against real audio.

---

## 5. Four defects the tests caught — read these before trusting a module

Each would have shipped, and each is the kind that fails quietly.

1. **`common_grapheme_prefix` returned 0 on mid-code-point divergence.** The byte-wise prefix
   landed on a continuation byte and `prev_code_point` refuses one. Effect: *nothing commits at
   all* for the rest of any utterance containing `café`/`cafè`, `и`/`й`, or two CJK characters
   sharing lead bytes. Fixed in `utf8.cpp` by flooring to a code-point boundary first;
   `tests/test_utf8.cpp` pins four such pairs.
2. **`at_offset` was one byte high** in the C++ JSON decoder — the byte that failed to *continue*
   a sequence rather than the one that *began* it. Every C++ unit test agreed with the bug because
   they shared its misunderstanding. Only fixture 008 disagreed. `utf8::validate` now returns both
   offsets and the header explains which is which.
3. **The sidecar could never have connected.** It opened the pipe with
   `GENERIC_READ | GENERIC_WRITE`; `GENERIC_READ` pulls in `FILE_READ_EA` (not granted →
   `ACCESS_DENIED` on every launch) and `GENERIC_WRITE` pulls in `FILE_CREATE_PIPE_INSTANCE` —
   the exact right plan.md §1.2 refuses to grant. The client was wrong, not the descriptor.
   `server/tests/test_protocol.py` now asserts the Python mask equals `kd::kPipeAccessRights` by
   parsing the header.
4. **The hotkey's binding key never left the down-at-activation set**, because an early return
   skipped the clearing that the comment three lines below it mandates.

Also worth knowing: eight test failures (four in `commit_engine`, four in Python `stability`) were
**mis-driven tests, not implementation bugs** — under-driven clocks and miscounted offsets. The
implementations were right every time. Do not assume a red test means the code is wrong here; the
modules have been more trustworthy than their tests.

---

## 6. Open questions recorded rather than guessed

These are written into the code and docs as unknowns. Do not let them quietly become assumptions.

* **`WDA_EXCLUDEFROMCAPTURE` vs a desktop-DC `BitBlt`.** The overlay excludes itself from capture
  so it cannot contaminate the anchor diff (plan.md §1.5). Whether the exclusion actually applies
  to a desktop-DC `BitBlt` is unverified. If it does not, the overlay pollutes every source-C
  measurement and Phase 0B's numbers are worthless.
* **Windows.Graphics.Capture desktop path**, and whether it shows a user-visible capture
  indicator. `make_wgc_capture` is deliberately unimplemented; `anchor.cpp` detects the all-black
  `BitBlt` that requires it and reports it. `capture.wgc_indicator_seen` stays `null` in probe
  records until an operator answers it.
* **WSLg + `KEYEVENTF_UNICODE`.** Whether Unicode survives translation to X11 keysyms across the
  RDP pipe. Clipboard is the likelier path. Phase 0B scenario A04 decides it.
* **Unicode version drift.** `utf8::kUnicodeVersion` is literally
  `"unspecified-pending-protocol-md-14.2"`. The C++ grapheme tables are hand-written and Python's
  `unicodedata` will drift from them. The fixtures constrain the cases that matter; the general
  problem is open. The real fix is generating both from one source.
* **`onnxruntime-gpu` on sm_120 (Blackwell)** — `bench/CANDIDATES.md` §6 could not confirm
  official Windows support; the upstream issue was closed stale. It gates nothing, because the
  chosen model runs faster than real time on CPU and this box has 28 threads.
* **`GetNamedPipeServerProcessId`** availability, for the child-validates-parent direction.

---

## 7. Suggested order

1. **Smoke test** (§4). Cheapest way to find out if the wiring works. Commit the fix.
2. **Turn on `KD_WERROR`.** The tree is at zero warnings; the original reason for leaving it off
   has expired. One line in `CMakeLists.txt`, then rebuild.
3. **Phase 0B** with `tools/probe --status` to see the matrix, then cell by cell. This is the gate
   that decides whether the inline overlay survives at all — plan.md is explicit that missing it
   changes both the UX and the commit policy, and that *not knowing* is the only failure.
4. **Fetch the ONNX model** (`bench/CANDIDATES.md` pick 1, a few hundred MB, no PyTorch) and make
   the sidecar real. Note `UV_CACHE_DIR` must be on `D:` — uv hardlinks from cache into venvs and
   hardlinks cannot cross volumes, so a `C:` cache with `D:` venvs silently full-copies everything.
5. **Record the corpus** with `bench/record.py` — but read its report first: it is written against
   the C++ capture path and may need a recording entry point that `audio_capture` does not yet
   expose. It deliberately does not import a Python audio library, because plan.md caught an
   earlier revision claiming a corpus recorded through a different chain was equivalent.

---

## 8. Conventions to preserve

* **Commit after each working step, directly to `main`.** Verify before committing — a commit
  should mark a state where the build and tests actually pass.
* No third-party C++ dependencies. No Qt. Everything in-repo, including the JSON parser.
* No header under `src/` includes `<windows.h>`; handles cross module boundaries as `uintptr_t`.
  This is what keeps the pure modules unit-testable, so do not "fix" it.
* `..\wan-dancer\.venv` is **read-only**. Run its Whisper baseline; install nothing into it.
* Comments explain *why*. Where something is unverified, the code says so rather than asserting.
* Empty test suites are registered `DISABLED` in `CMakeLists.txt` rather than omitted, so the gap
  between "planned" and "covered" shows up in every ctest run. `KD_TEST_SUITES_PENDING` is
  currently empty; a new module starts there.
