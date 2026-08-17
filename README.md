# kvit-dictate

A local, private replacement for Windows `Win+H`: press a hotkey, talk, text appears in whatever
field has focus. No cloud, no telemetry, no network access during dictation.

The authoritative design document is [`plan.md`](plan.md) (revision 2). The wire protocol is
[`docs/protocol.md`](docs/protocol.md). Everything below is a summary; where it disagrees with
those two, they win.

---

## What it is

`Win+H` fails on exactly the sentences this machine is used to dictate — `llama.cpp`, `Qwen3.6`,
`sm_120`, file paths, CLI flags — and offers no way to change its short, fixed silence timeout.
This is an attempt to do better, locally, with a model chosen by measurement rather than by
reputation.

Three design commitments shape everything else:

1. **Committed text is never repaired.** Every byte injected into the target must remain a prefix
   of every later hypothesis. Streaming ASR revises what it already said; `Win+H` can hide that
   because it owns its own text box, and we cannot, because we are typing into someone else's
   document. Replaying a revision means sending backspaces into an arbitrary application, where
   backspace can navigate a browser, dismiss an IDE autocomplete, or eat a terminal prompt. So
   revisions are absorbed in an overlay *before* commit, and a hypothesis that contradicts
   committed text is logged and counted, never applied. (`plan.md` §1.4)
2. **A late message can never reach the wrong window.** Every message carries a `session_id`, and
   anything naming a session that is not the current one is dropped — not clamped, not queued.
   (`docs/protocol.md` §7)
3. **Nothing is persisted and nothing is sent.** No transcripts or audio by default, no
   telemetry, no cloud fallback, no model auto-download at run time. Transcript logging is opt-in,
   retention-bounded, and compiled out of release builds entirely. (`plan.md` §1.9)

---

## Architecture: two processes, one contract

```
┌─────────────────────────────┐        ┌──────────────────────────────┐
│  kvit-dictate.exe (C++)     │        │  asr-server (Python)         │
│                             │        │                              │
│  • global hotkey            │ named  │  • one backend per process   │
│  • WASAPI mic capture       │ pipe   │  • VAD                       │
│  • session state machine    │◄──────►│  • streaming decode          │
│  • anchor estimation        │ binary │  • stability computation     │
│  • commit engine            │ frames │  • partial/final emission    │
│  • overlay / injection      │ + JSON │                              │
│  • tray + settings          │        │  (sherpa-onnx | HF | CT2)    │
└─────────────────────────────┘        └──────────────────────────────┘
```

**Why split.** Every candidate model has a different runtime — sherpa-onnx, HuggingFace
`transformers`, CTranslate2, NeMo — and several need their own locked virtual environment. In
Python each new candidate is a ~50-line adapter; in C++ it is weeks of work per model, spent
before we know which one we keep. The C++ side never learns what the model *is*: the
model→interpreter map lives in [`models.json`](models.json) and selecting a model restarts the
sidecar under that model's interpreter.

**One confusing thing, stated up front.** `kvit-dictate.exe` is the *pipe server* (it creates the
pipe) but the *protocol client* (it drives sessions). `asr-server` is the *pipe client* but the
*protocol server*. Only the parent can generate a per-launch random name, apply a DACL before
anyone connects, and know the child's PID for peer validation — hence the inversion.

### Source layout

```
src/
  main.cpp            tray, message loop, lifecycle          (the single-threaded serialisation point)
  protocol.{h,cpp}    framing + message codec                ← pure, unit-tested, no Windows
  session.{h,cpp}     session/utterance state machine        ← pure, unit-tested
  commit_engine.{h,cpp} stability → injection ops            ← pure, unit-tested
  ipc_client.{h,cpp}  pipe, DACL, child spawn, framed I/O
  audio_capture.{h,cpp} WASAPI → 16 kHz mono s16            (AudioConverter half is pure and tested)
  anchor.{h,cpp}      3-source confidence-scored anchor      (diff_bitmaps/score_diff are pure)
  injector.{h,cpp}    SendInput / clipboard; UIPI + focus guards
  overlay.{h,cpp}     layered, click-through, capture-excluded
  hotkey.{h,cpp}      WH_KEYBOARD_LL + WH_MOUSE_LL           (InputFilter is pure and tested)
  settings.{h,cpp}    settings + the models.json manifest
  util/               utf8, json, log, result, clock         ← all pure
tools/probe/          Phase 0B spike: fixed-text injector + anchor probe
tools/testtarget/     controlled target: edit, richedit, multiline, password, RTL
tests/                kd_test harness + suites + shared protocol fixtures
server/kvit_asr/      Python sidecar: transport, session, stability, vad, backends/
  connection.py       the connection-scoped layer joining server.py to session/backends/vad
bench/                corpus recorder, experiment matrix, metrics
```

**A convention worth knowing before reading any header:** no header in `src/` includes
`<windows.h>`. Windows handles cross module boundaries as `uintptr_t`, and geometry as the plain
structs in `anchor.h`. This is not portability theatre — the project is Windows-only and always
will be — it is what keeps the pure-logic modules compilable and testable without dragging the
Win32 surface into every test translation unit, and what makes "which parts are testable" a
mechanical question rather than a judgement call.

---

## Building

Requirements, all already present on the target machine (`plan.md` §5.3):

* Visual Studio 2022 Community with the C++ toolchain (MSVC 14.44) and Windows SDK 10.0.26100
* CMake ≥ 3.24 at `C:\Program Files\CMake\bin\cmake.exe`

There are **no third-party C++ dependencies** — no Qt, no nlohmann/json, no Catch2, no fmt. If a
build step ever wants to download something, that is a bug.

Ninja is not on PATH here, so the presets use the Visual Studio generator (as `kvit-notes` does).

```bat
rem everything: configure, build, test
win-build.bat

rem release
win-build.bat Release

rem from scratch
win-build.bat Debug clean
```

Or by hand:

```bat
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
ctest --test-dir build-windows-msvc-debug -C Debug --output-on-failure
```

Binaries land in `build-windows-msvc-debug\bin\Debug\`.

---

## Tests

The C++ suite uses `kd_test`, a ~300-line self-registering framework in
[`tests/kd_test.h`](tests/kd_test.h). Adding a test means writing `KD_TEST(suite, name) { ... }`
in a file listed in `KD_TEST_SOURCES` — there is no registration list, no fixture base class, and
no `main()` to write.

```bat
build-windows-msvc-debug\bin\Debug\kd_tests.exe             rem everything
build-windows-msvc-debug\bin\Debug\kd_tests.exe --list      rem what exists
build-windows-msvc-debug\bin\Debug\kd_tests.exe --suite utf8 --verbose
```

`ctest` registers the whole suite as `kd_tests.all` plus one entry per suite. **Suites with no
tests yet are registered as `DISABLED`**, so every `ctest` run prints the gap between what is
planned and what is covered instead of that gap living only in a document. Moving a suite from
`KD_TEST_SUITES_PENDING` to `KD_TEST_SUITES_READY` in `CMakeLists.txt` is the whole ceremony for
enabling one.

An empty run is a failure: `kd_tests` exits non-zero if a filter matched nothing, because a typo
that silently runs zero tests is how a green build stops meaning anything.

### Cross-language conformance fixtures

`tests/fixtures/protocol/` holds 20 cases read by **both** the C++ suite and the Python suite.
Neither may keep a private copy: the point is that a disagreement between the two implementations
shows up as a failing shared fixture rather than as garbled text in someone's document
(`docs/protocol.md` §12). The C++ runner lives in `tests/test_fixtures.cpp`.

### Python tests

The C++ build never depends on Python. The `pytest` target is registered only when the marker
file `server\.pytest-enabled` exists:

```bat
type nul > server\.pytest-enabled
```

The marker is gitignored — whether this machine has a Python environment is a local fact.

---

## Status

**The vertical slice works, with the mock backend.** Press the hotkey in Notepad and text is typed
into it as the hypotheses stabilise; press it again and the final's remaining tail is appended,
once — verified by running it, after nine defects that no test had caught. What has still never
happened is the part that needs a model and a measurement: no real speech has been transcribed, no
model downloaded, no corpus recorded, and none of the Phase 0B measurements that decide the
product's defining interaction have been taken. The inline overlay has not yet been observed doing
its job.

| Phase (`plan.md` §3) | State |
|---|---|
| **M-1** Environment resolved | ✅ done — torch 2.10.0+cu130 / sm_120 verified; `..\wan-dancer\.venv` declared read-only |
| **0A** Source and environment verification | ✅ done — `bench/CANDIDATES.md`; all three PRD citations resolve; four Phase-1 picks selected; NeMo need not be installed |
| **Protocol spec** | ✅ done — `docs/protocol.md` (proto 1 draft) + 20 fixtures, run by BOTH endpoints. Freezes in Phase 2. |
| **C++ client** | ✅ implemented — every module under `src/`, wired together in `main.cpp`. 299 tests. The whole path has now been executed against a real target: hotkey, microphone, session, commit, injection, finalisation. |
| **Python sidecar** | ✅ implemented — transport, VAD, stability, session, backends (mock, sherpa-onnx, zipformer, whisper, faster-whisper), joined by `kvit_asr/connection.py`. 300 tests. Only `mock` is registered in the backend REGISTRY. |
| **Bench harness** | ✅ implemented — recorder, metrics (incl. ICER), matrix runner with the pre-registered selection rule. 129 tests. |
| **0B** Windows feasibility spike | ⚠️ **instrument ready, not run — this is the critical gate.** `tools/probe` builds and walks a resumable 162-cell matrix; 0 cells are measured. Until an operator runs it, the inline-overlay UX is unproven and `docs/supported-targets.md` contains no measurements. |
| **1** Production audio path + offline bake-off | ⚠️ path built and unit-tested (including the anti-alias response); no corpus recorded, no model run |
| **2** Protocol freeze + streaming bake-off | ❌ not started — needs a model and a corpus |
| **3** Minimal vertical product | ⚠️ runs end to end with the mock backend; never exercised against a real ASR backend, and the overlay's inline preview is unobserved |
| **4** Productisation | ❌ not started |
| **5** Dogfood | ❌ not started |

### What has and has not been verified

Verified by tests that run: the framing codec against a shared fixture corpus; the committed-prefix
invariant, including that cadence cannot promote unstable text; the resampler's frequency response
(a 12 kHz tone is attenuated rather than folded back to 4 kHz); the pipe DACL excluding Everyone and
`FILE_CREATE_PIPE_INSTANCE`; the anchor diff scorer against synthetic bitmaps including the
line-wrap and scroll counterexamples; the session machine's staleness and self-stop rules.

NOT verified, and not claimable until someone runs it: that any real ASR model loads and streams;
that injection is exact in any real target application; that the anchor lands within one line
height anywhere; that the WGC fallback works at all (its desktop path is still an open question);
that a WSLg window accepts `KEYEVENTF_UNICODE`. Every one of these is a Phase 0B or Phase 1
measurement, and the repository states them as unmeasured rather than assumed.

One build setting is deliberately not final and is called out here so it does not become permanent
by inertia:

* **No application manifest yet.** `PROCESS_PER_MONITOR_DPI_AWARE_V2` must come from a manifest
  (it applies before any window exists) and that lands with the first real window in Phase 3.

**Warnings are errors** (`KD_WERROR=ON`, since the vertical slice ran). Debug and Release both
build at `/W4` with zero warnings, so the next warning is a new one. If a different Windows SDK
produces warnings this one does not, that is the moment to look at them, not to turn the flag off.

---

## Reading order

1. [`plan.md`](plan.md) — the design, and the reasoning behind each decision that could have gone
   another way. §1.4 (the commit invariant) and §1.5 (anchor estimation, including an honest list
   of where it fails) are the two that most affect how the code reads.
2. [`docs/protocol.md`](docs/protocol.md) — normative for both endpoints.
3. [`bench/CANDIDATES.md`](bench/CANDIDATES.md) — which models are real, what they need, and why
   the shortlist is four entries.
4. `src/*.h` — each header states its responsibility, the `plan.md` section it implements, and its
   **threading contract**. The threading contracts are not boilerplate: the low-level keyboard
   hook must never block or Windows silently unhooks it, the WASAPI callback runs on its own
   thread under a latency budget, and the pure state machines are single-threaded on purpose.
