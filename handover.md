# Handover — kvit-dictate

Rewritten 2026-08-16, end of the second session. Commit `a29d82f` on `main`.

Read this with `plan.md` (the specification), `README.md` (current status) and
`docs/protocol.md` (the wire contract). This file covers only what those three do not: where the
work actually stands, what is untrue about it, and what to do next.

---

## 1. State in one paragraph

The two processes now talk to each other. `kvit-dictate.exe` spawns the sidecar under the
interpreter `models.json` names, both ends validate the peer, the handshake completes, the mock
backend loads and `ready` arrives — a path that had never once completed before this session and
was broken in five separate places. 725 tests pass across four suites at zero warnings, with
warnings now errors. **Nothing has still ever transcribed real speech, no model has been
downloaded, no Phase 0B cell has been measured, and no text has ever been injected into a real
window** — the hotkey→microphone→commit→SendInput half of `main.cpp` is the largest piece of code
that has not run, because running it means synthesising keystrokes into whatever has focus.

---

## 2. How to build and test

```
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
build-windows-msvc-debug\bin\Debug\kd_tests.exe            # 296 tests / 35807 checks
cd build-windows-msvc-debug && ctest -C Debug              # 14 suites, none disabled

python -m unittest discover -s server/tests -t server      # 300 tests  (run from repo root)
python -m unittest discover -s bench/tests  -t .           # 129 tests  (run from repo root)
```

Use `C:\Python312\python.exe`. The `python` on PATH in Git Bash is a conda 3.9 and will fail.

Working notes that cost time to rediscover:

* `cmake` is at `C:\Program Files\CMake\bin\cmake.exe`; Ninja is NOT on PATH, so the VS 2022
  generator is the only one configured.
* The two Python suites must be run **from the repository root**; `-t` sets the import root and
  running from inside `server/` or `bench/` fails with "Start directory is not importable".
* `kd_tests.exe --list` prints every test; `--suite <name>` filters.
* `KD_WERROR` is now **ON**. Debug and Release both build the whole tree at zero warnings.
* **Kill any running `kvit-dictate.exe` before rebuilding.** It holds the link output *and* the
  single-instance mutex, so a stale one turns into `LNK1168` plus a new instance that exits
  immediately — two confusing symptoms of one cause.

---

## 3. What is done, and what that means

| Area | State |
|---|---|
| `docs/protocol.md` + 20 fixtures | Written, and **both endpoints execute the same corpus**. |
| C++ modules | All 13 implemented. 296 tests. |
| `src/main.cpp` | **Runs.** Startup, tray, sidecar launch, handshake, model load and the settings dialog have all been executed. The dictation half has not. Still no test touches it. |
| Python sidecar | Implemented and integrated: `kvit_asr/connection.py` is the connection-scoped layer that was missing. 300 tests. |
| Bench harness | Recorder, metrics (incl. ICER), matrix runner with the pre-registered selection rule. 129 tests. |
| `tools/testtarget` | Implemented — the controlled app with edit/richedit/password/multiline controls that logs the code points it receives. Never run. |
| `tools/probe` | Implemented, builds, walks a resumable 162-cell matrix. **0 cells measured.** |

### Explicitly NOT done

* **No model downloaded.** `bench/CANDIDATES.md` picked the shortlist; nothing was fetched.
  plan.md §1.9 makes model fetching a deliberate, user-initiated act, so this is correct, not an
  oversight — but every backend except `mock` is still untested against reality. Only `mock` is
  even registered in `backends/__init__.py`'s REGISTRY; the other four modules exist but cannot be
  named on the command line until they are added there.
* **No environments created.** `envs/` does not exist. The mock entry no longer needs one — it
  points at `C:\Python312\python.exe`, because the mock backend is stdlib-only.
* **No corpus recorded.** `bench/prompts/{tech,hard,ambient}.jsonl` contain 91 real prompts with
  the dev/heldout split baked into the data; no audio exists.
* **Phase 0B not run** — the critical gate. `docs/supported-targets.md` is all `—`.
* **`Hypothesis.stable_bytes_override` is not honoured.** `backends/base.py` documents it as the
  hook a deliberately hostile mock uses to force an illegal `stable_bytes` onto the wire, so that
  the C++ side's §8.3 violation handling can be exercised end to end; `connection.LoadedBackend`
  passes only text across the seam, and `session.Session` computes stability itself. The C++
  handling is covered by fixtures, so nothing is unverified — but the end-to-end hostile path
  described in `models.json`'s mock notes does not exist yet.

---

## 4. Do this first: dictate one sentence

Everything from the hotkey to the injected character is untested and unexecuted. It needs a human,
because it installs a global low-level keyboard hook and synthesises keystrokes into whatever
window has focus.

```
build-windows-msvc-debug\bin\Debug\kvit-dictate.exe
```

Then, with Notepad (or `kd_testtarget.exe`, which logs the code points it receives) focused, press
`Win+Y`, speak, and press `Win+Y` again. With the `mock` backend the text is a scripted sentence
and does not depend on what you say — which is the point: a failure is then attributable to the
C++ side rather than to a model.

Turn the log on first, or there is nothing to read afterwards:
`%LOCALAPPDATA%\kvit-dictate\settings.json`

```json
{ "schema": 1, "privacy": { "log_level": "debug", "diagnostics_file_enabled": true } }
```

The log lands in `%LOCALAPPDATA%\kvit-dictate\logs\kvit-dictate.log`.

Things most likely to be wrong, in order:

1. **The VAD gate.** The sidecar defaults to `--vad auto`, which finds no Silero model and falls
   back to the stdlib energy endpointer. That fallback calibrates its noise floor from the
   quietest frame of the opening 200 ms, so if you start talking the instant the session opens it
   calibrates the floor to your voice and hears nothing at all. Leave a moment of silence after
   the hotkey. If it still hears nothing, add `"--vad", "none"` to the mock entry's `args` in
   `models.json` to take the detector out of the picture entirely.
2. **The `Effect` dispatcher in `App::apply`** — the effect ordering is implemented from
   `session.h`'s contract and has never been observed.
3. **`Effect::ApplyText` carries no payload**; `main.cpp` passes the in-flight message alongside
   the `Outcome`. If a code path reaches `ApplyText` without a message, text is silently dropped.
4. **Timer cadence.** `on_tick` runs on a 50 ms `WM_TIMER`, which drives both silence timers and
   the commit cadence. Nothing has validated that interval against real audio.
5. **The commit engine's stability dwell against a real clock.** Every test of it runs on a
   manual or ticking clock. The sidecar's `Session.tick()` is driven by audio arrival (see
   `connection.ConnectionSession.handle_audio` for why it is not a timer thread), so if the client
   ever stops streaming during a session, `stable_bytes` stops advancing.

---

## 5. Defects found by running it, and by the tests

The first four are from the previous session, kept because each is still the kind that fails
quietly. The rest are this session's, and every one of them was found by *running the thing* —
none was visible to 691 passing tests.

1. **`common_grapheme_prefix` returned 0 on mid-code-point divergence**, so nothing would commit
   at all for the rest of any utterance containing `café`/`cafè`, `и`/`й`, or two CJK characters
   sharing lead bytes. Fixed in `utf8.cpp`; `tests/test_utf8.cpp` pins four such pairs.
2. **`at_offset` was one byte high** in the C++ JSON decoder. Every C++ unit test agreed with the
   bug because they shared its misunderstanding; only fixture 008 disagreed.
3. **The sidecar could never have connected** — it opened the pipe with `GENERIC_READ |
   GENERIC_WRITE`, which asks for rights the DACL deliberately does not grant.
4. **The hotkey's binding key never left the down-at-activation set.**
5. **Two authors for `hello`.** `IpcClient` wrote the handshake's first frame *and* so did
   `SessionMachine`, which is the half the frozen fixture corpus tests. Nothing forwarded the
   transport's walk to the machine, so it sat in SPAWNING and rejected the `hello_ack` as a state
   violation on every launch, on every respawn, forever. The machine keeps the frame; `main.cpp`
   maps connection states onto `TransportEvent::Connected` / `PeerValidated` / `ChildRespawned`.
6. **The sidecar was launched without `-m <module>`** and without the package on its PYTHONPATH,
   so it died on an unknown option before the pipe mattered. `main.cpp` also never applied a
   model entry's `env`, never fell back to `models.json`'s `default_model` (so a first run with no
   settings file started no sidecar at all), and never gave `log::Config` a file even when the
   settings said to.
7. **`decode_control` returned a `Message`; `server.py` speaks dicts.** The sidecar's handshake
   died on `'Hello' object has no attribute 'items'`. It could only ever have worked against the
   test suite's shim.
8. **Every `error.code` the sidecar raised as a `ProtocolError` reached the wire spelled
   `"ErrorCode.state_violation"`** — outside §11.3's catalogue, which §13 tells a receiver to
   treat as an error about an error. `tests/test_server.py` had already pinned this as an
   `expectedFailure`; the fix is `ProtocolError.code_raw`.
9. **Nothing connected `server.py` to `session.py`, `backends/` or `vad.py`.** Four modules, four
   different plausible interfaces, no owner for the space between them: connection-scoped versus
   session-scoped, `feed`/`finalize`/`Hypothesis` versus `accept_audio`/`finalise`/`str`, absolute
   backdated sample positions versus byte offsets into the current chunk. `kvit_asr/connection.py`
   is those three translations and nothing else. The test suite had carried all three as shims and
   named them, which is the only reason this took an afternoon rather than a week.
10. **`FindWindowW` cannot find a message-only window**, so the single-instance path returned null
    every time and a second launch exited in silence — the exact "double-click does nothing"
    outcome it was written to avoid. `FindWindowExW(HWND_MESSAGE, …)` is the search that works.
11. **The settings dialog was unreachable.** ~200 lines of programmatic Win32 that nothing called.

Also worth knowing, from the first session: eight test failures (four in `commit_engine`, four in
Python `stability`) were **mis-driven tests, not implementation bugs**. Do not assume a red test
means the code is wrong here.

---

## 6. Open questions recorded rather than guessed

* **`WDA_EXCLUDEFROMCAPTURE` vs a desktop-DC `BitBlt`.** The overlay excludes itself from capture
  so it cannot contaminate the anchor diff (plan.md §1.5). Whether the exclusion actually applies
  to a desktop-DC `BitBlt` is unverified. If it does not, the overlay pollutes every source-C
  measurement and Phase 0B's numbers are worthless.
* **Windows.Graphics.Capture desktop path**, and whether it shows a user-visible capture
  indicator. `make_wgc_capture` is deliberately unimplemented; `anchor.cpp` detects the all-black
  `BitBlt` that requires it and reports it.
* **WSLg + `KEYEVENTF_UNICODE`.** Whether Unicode survives translation to X11 keysyms across the
  RDP pipe. Phase 0B scenario A04 decides it.
* **Unicode version drift.** `utf8::kUnicodeVersion` is literally
  `"unspecified-pending-protocol-md-14.2"`. The real fix is generating both tables from one source.
* **`onnxruntime-gpu` on sm_120 (Blackwell)** — unconfirmed; gates nothing, because the chosen
  model runs faster than real time on CPU.
* **The energy VAD's noise-floor warmup** (new). It is deaf to speech that starts within the first
  200 ms of a session. Silero would not have this wart. Whether it matters in practice is a
  measurement nobody has taken; `tests/test_connection.py` pins the behaviour so at least it is
  not a surprise.
* **Pre-roll is dropped across the VAD seam** (new). `VadEndpointer` replays `preroll_ms` of audio
  from before a detected onset; `session.Session` gates by splitting the chunk it was given, so
  that audio is never fed. The cost is the leading fraction of the first word. Fixing it means
  teaching `Session` to consume the VAD's audio instead of its own — a change to a tested module,
  and it belongs in the same pass as measuring whether it matters.

---

## 7. Suggested order

1. **Dictate one sentence** (§4). It is the only thing standing between "the processes talk" and
   "the product does something".
2. **Phase 0B** with `tools/probe --status` to see the matrix, then cell by cell. This is the gate
   that decides whether the inline overlay survives at all — plan.md is explicit that missing it
   changes both the UX and the commit policy, and that *not knowing* is the only failure.
3. **Fetch the ONNX model** (`bench/CANDIDATES.md` pick 1, a few hundred MB, no PyTorch), register
   the backend in `backends/__init__.py`'s REGISTRY, and make the sidecar real. Note `UV_CACHE_DIR`
   must be on `D:` — uv hardlinks from cache into venvs and hardlinks cannot cross volumes, so a
   `C:` cache with `D:` venvs silently full-copies everything.
4. **Record the corpus** with `bench/record.py` — but read its report first: it is written against
   the C++ capture path and may need a recording entry point that `audio_capture` does not yet
   expose.

---

## 8. Conventions to preserve

* **Commit after each working step, directly to `main`.** Verify before committing — a commit
  should mark a state where the build and tests actually pass.
* No third-party C++ dependencies. No Qt. Everything in-repo, including the JSON parser.
* No header under `src/` includes `<windows.h>`; handles cross module boundaries as `uintptr_t`.
  This is what keeps the pure modules unit-testable, so do not "fix" it.
* The sidecar core is stdlib-only. Only individual backends may import numpy/onnxruntime/torch,
  and each such import lives inside the backend module, so a missing runtime disables one backend
  rather than the server.
* `..\wan-dancer\.venv` is **read-only**. Run its Whisper baseline; install nothing into it.
* Comments explain *why*. Where something is unverified, the code says so rather than asserting.
* Empty test suites are registered `DISABLED` in `CMakeLists.txt` rather than omitted, so the gap
  between "planned" and "covered" shows up in every ctest run. `KD_TEST_SUITES_PENDING` is
  currently empty; a new module starts there.
