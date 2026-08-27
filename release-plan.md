# kvit-dictate — Release Plan (v1.0, first build for other people)

Status: **draft, nothing here is built yet.** Written 2026-08-27 against commit `5e8d7d8`.

`plan.md` is the design document for the experimental phase and it still wins on every question
of architecture. This document is narrower and later: it covers only what has to change for
someone who is not the author to install this and use it. It also deliberately **reverses one
line of `plan.md` §6**, which lists "installer/auto-update" among the non-goals. That was the
right call for the experiment; it is the wrong call now, and reversing it is the point of this
file.

---

## 0. Scope, stated before anything else

**In:** Windows 10 (1809+) and 11, x64. One installer, one settings page where a user chooses an
engine and types a server URL, no Python anywhere on the user's machine, a README that teaches a
stranger to use the thing.

**Out, and not negotiable for v1:**

* **macOS.** This came up in the request and it deserves a straight answer rather than a
  placeholder: the client is not portable and porting it is not a packaging task. `src/` is
  `WH_KEYBOARD_LL` (hotkey.cpp), WASAPI (audio_capture.cpp), `SendInput` + UIPI integrity
  comparison (injector.cpp), UI Automation and `BitBlt`/WGC caret hunting (anchor.cpp),
  DirectWrite + layered windows (overlay.cpp), Shell_NotifyIcon (tray_icon.cpp), DPAPI, WinHTTP,
  named pipes with logon-SID DACLs (ipc_client.cpp). `CMakeLists.txt:20` refuses to configure off
  Windows and says why. On macOS every one of those is a different API with different rules —
  and the two hardest ones, global hotkeys and synthetic keystrokes into another app, are gated
  behind Accessibility and Input Monitoring permissions that change the UX shape of the product.
  A Mac version is a **second client against the same wire protocol** (`docs/protocol.md`), which
  is a real and attractive project, and it is not this one. What *is* in scope here helps it:
  everything in §1 below moves ASR behind a protocol boundary that a future Mac client can speak.
* **Auto-update.** Ship a version check that points at a download page. A silent updater is a
  second signed-binary supply chain and it can wait for v1.1.
* **The research backends.** `faster_whisper`, `whisper_transformers`, `sherpa_onnx_streaming`,
  `zipformer_hotwords` (4,272 lines of Python between them) stay exactly where they are, for the
  bench. They are not shipped and users never learn they exist.

---

## 1. Getting Python off the user's machine

### 1.1 What the dependency actually is today

The client does not contain an ASR engine. It spawns one:

* `models.json` maps a model id to an **absolute path to a `python.exe`** plus a module and args.
  `src/settings.cpp:436-448` *rejects the manifest at parse time* if that path is not an absolute
  path to a Python interpreter — this is enforced, not conventional.
* `src/ipc_client.cpp:1243-1306` builds a command line from it and calls `CreateProcessW`; the
  two processes then speak `docs/protocol.md` over a named pipe.
* `server/kvit_asr/` is **14,966 lines of Python** doing the endpointing (`vad.py`, 1,204), the
  stable-prefix computation (`stability.py`, 871), the framing and the wire types
  (`transport.py` + `protocol.py`, 2,691), the session routing (`session.py` + `connection.py` +
  `server.py`, 2,549) and the backend adapters.

Every shipped entry in `models.json` today also hardcodes something that exists only on this
machine: `C:\Python312\python.exe`, `D:\projects\kvit-dictate\envs\onnx\Scripts\python.exe`,
`D:\projects\wan-dancer\.venv\Scripts\python.exe`, and `http://strixhalo.lan:10301` as the
default endpoint (`server/kvit_asr/backends/openai_http.py:95`). A stranger's machine has none of
those.

### 1.2 Decision: keep the protocol boundary, replace the process

Ship **`kvit-asrd.exe`** — a native C++ sidecar that speaks the same `docs/protocol.md` the
Python one does, built from this repo, installed beside `kvit-dictate.exe`.

Why this rather than folding ASR into the client:

* The protocol boundary is the most thoroughly specified thing in the project and the client half
  is already written and tested — `protocol.cpp`, `session.cpp`, `commit_engine.cpp` and 479
  passing tests do not change at all. A rewrite that dissolves the boundary throws that away and
  re-earns it.
* Crash isolation is load-bearing. `App::on_sidecar_lost` (`src/main.cpp:2195`) already handles a
  dead engine by keeping committed text and losing only the tail. An in-process ONNX runtime that
  faults takes the user's dictation and the tray icon with it.
* The Python sidecar keeps working, unchanged, for the bench and for anything with weights. §4 of
  `plan.md` is a measurement programme and it must not be collateral damage.
* A future Mac client (§0) gets a defined thing to talk to.

**Rejected: freezing the Python sidecar with PyInstaller.** It produces a 300 MB+ bundle for the
ONNX backends, a startup penalty on every session, and — combined with a global keyboard hook and
synthetic input — a packed Python interpreter is close to the worst possible shape to hand to
Defender heuristics (see §4).

### 1.3 What the native sidecar has to contain, for v1 only

| Python module | Lines | v1 disposition |
|---|---|---|
| `transport.py`, `protocol.py` | 2,691 | **Port.** The client half already exists in `src/protocol.cpp`; this is the server half against the same spec, and the same fixtures test both. |
| `connection.py`, `session.py`, `server.py` | 2,549 | **Port the routing only.** Much of this is CLI surface and backend-selection machinery a shipped exe does not need. |
| `stability.py` | 871 | **Port.** Non-negotiable: this computes `stable_bytes`, and §8.2 of the protocol is what stops the client typing text that later moves. |
| `vad.py` | 1,204 | **Port, reduced.** See §1.4 — this is the one place the plan is genuinely uncertain. |
| `backends/openai_http.py` | 407 | **Port to WinHTTP.** `src/normalizer.cpp` already does exactly this shape of thing (POST JSON to a configured endpoint, on a worker, cancellable, with a "test this now" path), so it is a pattern to copy, not to invent. |
| `backends/windows_speech.py` + `tools/kvit-speech` (C#) | 561 + C# | **Port to C++/WinRT** — or ship the C# tool as-is for v1. See §1.5. |
| `backends/mock.py` | 803 | **Stays Python.** It exists to violate the protocol deliberately; it is a test fixture and must never be in a shipped build. |
| `faster_whisper`, `whisper_transformers`, `sherpa_*`, `zipformer_*` | 4,272 | **Stay Python, not shipped.** |

### 1.4 The VAD question — the one real unknown

`vad.py` is the endpointer: it decides when an utterance ends, 32 ms at a time, and its quality is
directly visible to the user as "it cut me off mid-sentence" or "it never stops". The Python path
can reach Silero (an ONNX model); a native sidecar that wants the same quality needs
`onnxruntime`, which collides head-on with `CMakeLists.txt:1-9` — *"no third-party dependencies of
any kind … there is no `find_package()` call in this file and there must not be one"*.

Three ways out, to be decided in R1 with a measurement, not an opinion:

1. **Energy + hangover VAD in the sidecar** (zero dependencies). Cheap, and probably adequate
   *because the endpoint silence is already user-configurable at 1500 ms* — the shipped
   configuration is not trying to be clever. Risk: noisy rooms.
2. **Server-side VAD**, which the HTTP engine already supports and already uses:
   `vad_gate=true` asks the same host's `/v1/audio/vad`, costs 50–176 ms, and fails open. Free
   for the remote engine, useless for the on-device one.
3. **Vendor `onnxruntime` for the sidecar only**, leaving `kvit-dictate.exe`'s no-dependency rule
   intact. Honest, and it adds ~15 MB and a second signing target.

Recommended: build (1), keep (2) on for the HTTP engine, and treat (3) as a v1.1 upgrade with the
measurement in hand. **Do not ship the sidecar without a false-endpoint measurement against real
speech**, because this is the failure users describe as "it's broken" without ever saying VAD.

### 1.5 Windows AI Speech: ship it or defer it

`tools/kvit-speech` already exists — a C# helper that reaches `Microsoft.Windows.AI.Speech`
through the Windows App SDK **experimental** channel, because the WinRT projection is absent from
stable 2.4.0. It works, and `models.json` records its three measured caveats honestly (≈1350 ms
fixed decode cost, an experimental API, hallucination over silence).

This is the only engine that keeps `plan.md` §2's "no network during dictation" promise intact,
so v1 wants it. But it drags in a .NET 9 runtime, an experimental SDK pin, a Windows 11 24H2
build floor, and an optional Windows component the user has to fetch (`--ensure-model`).

**Recommendation:** v1 ships the HTTP engine as the supported path and Windows AI Speech as a
clearly-labelled second option that the settings page **detects rather than assumes** — if the
build is below 26100 or the model is absent, the option is greyed with a sentence saying which
one, in the style of the existing "listed but not installed" handling in
`check_model_launchable` (`src/settings.h:141`). Port C# → C++/WinRT in v1.1 if it earns its keep.

---

## 2. What a user configures, and where

### 2.1 The gap

There is no way for a user to type a server URL. The endpoint is a Python module constant; the
model choice is a `models.json` id; `Settings` (`src/settings.h:157`) has 20-odd fields and not
one of them is an engine address. Everything about the current configuration assumes the person
running it also wrote it.

### 2.2 Work

* **`models.json` schema 2.** `interpreter` becomes optional; entries gain a kind
  (`builtin-http`, `builtin-windows-speech`, `python`). Schema-1 files keep parsing — the dev
  entries in this repo must survive untouched. Touches `parse_model_manifest`
  (`src/settings.cpp:411+`), whose current absolute-python-path check is the thing in the way.
* **`EngineConfig` on `Settings`**: `base_url`, `model`, `api_key`, `timeout_ms`, `finalize_only`,
  `vad_gate`. Persisted like the rest, **except the key**.
* **The API key does not go in `settings.json` in plaintext.** DPAPI (`CryptProtectData`,
  `CRYPTPROTECT_UI_FORBIDDEN`) with the ciphertext base64'd into the settings file. The project
  already takes this class of thing seriously — logon-SID pipe DACLs, transcripts compiled out of
  release builds — and a plaintext key in `%LOCALAPPDATA%` would be the loosest thing in it.
* **An "Engine" page in the settings dialog** (`src/settings.cpp:1517+`): a radio for the engine,
  URL and key fields, model name, and a **Test** button that performs one real round trip and
  reports the outcome and the latency. The normalizer's on-demand WinHTTP test is the precedent
  and it answers in 295 ms; a settings page that can prove the endpoint works turns the entire
  "it doesn't work" support burden into one button.
* **First run.** With no `settings.json`, the current default comes from `models.json`
  `default_model`, which is `"mock"` — a scripted fixture. A shipped build must not start there:
  first run opens the Engine page with nothing selected and a one-paragraph explanation, and the
  tray says "not configured" rather than pretending to listen.
* **Defaults that respect the invariant.** `models.json` records that the HTTP engine produced a
  **committed-prefix violation within seconds** of its first human run (2026-08-18), structurally,
  because a batch model re-punctuates earlier words as later context arrives. So v1 ships
  `finalize_only=true` and end-of-session cadence, and the live-preview mode is available but
  labelled for what it is.

---

## 3. The installer

### 3.1 Choice

| Option | Verdict |
|---|---|
| **Inno Setup, per-user** | **Recommended.** Installs to `%LOCALAPPDATA%\Programs\kvit-dictate` with no admin prompt, which matters because the first thing this app does is install a global keyboard hook — an elevation prompt on top of that is exactly the wrong first impression. Free, one `.iss` script, signs its own uninstaller. |
| MSIX | Attractive on paper (clean uninstall, App Installer updates) and **needs a spike before anyone commits to it**: the app's whole job is a low-level keyboard hook, synthetic input into arbitrary windows, and UI Automation/`BitBlt` against other processes' windows. Full-trust packaged apps can do this, but Store certification would not accept it and the packaged-identity edge cases here are unknown. Not v1. |
| WiX / MSI | The right answer if this is ever deployed by an enterprise via GPO. Per-machine, admin, the steepest authoring curve of the three. Defer until someone asks. |
| Portable ZIP | Ship it alongside, it costs nothing and it is what a suspicious user tries first. |

### 3.2 Contents, and two build changes it forces

Payload: `kvit-dictate.exe`, `kvit-asrd.exe`, `models.json`, `LICENSE`, `README`.

* **The CRT.** The build is `/MD` today and `CMakeLists.txt:96-98` says static linking was left
  alone because *"this is not a redistributable yet"* and would have to be revisited for signing.
  This is that moment. Switch to `/MT`, or bundle the VC++ redistributable — `/MT` for two small
  exes is simpler and removes an installer step that fails on locked-down machines.
* **The application manifest**, still a `TODO(phase-3)` at `CMakeLists.txt:243`: PerMonitorV2 DPI
  awareness and `requestedExecutionLevel asInvoker`. The DPI half is a correctness bug on scaled
  monitors (`anchor.cpp:801` already warns at runtime when it is missing); the `asInvoker` half
  must be explicit, because `plan.md` §1.7 makes elevated targets an *unsupported state* and the
  app must never be the thing that elevates.

Also needed: an icon (there is none), a version resource, a Start Menu shortcut, an optional
run-at-login checkbox wired to the existing `Settings::start_with_windows`, and an uninstaller
that leaves `%LOCALAPPDATA%\kvit-dictate\settings.json` alone unless the user ticks "also remove
my settings".

---

## 4. Signing and Defender — the biggest risk in this document

`plan.md` §6 already called it: *"Antivirus flags keyboard hook + synthetic input — expected, this
looks like a keylogger to heuristics. Sign early; test under Defender before Phase 4 closes."*
That risk has been carried, unmitigated, the whole way here. An unsigned binary that hooks the
keyboard and synthesises input, downloaded from the internet, is a SmartScreen warning at best and
a silent quarantine at worst — and a user who gets either one never files a bug, they just leave.

Plan:

1. **Get a certificate.** Since June 2023 code-signing keys must live in certified hardware, so
   the practical options are **Azure Trusted Signing** (subscription, no HSM to manage, needs an
   organisation with 3+ years of verifiable history) or an **OV certificate on a hardware token**
   from a CA. An **EV** certificate is the only thing that grants SmartScreen reputation
   immediately; OV builds reputation over download volume, which for a niche tool can take a long
   time.
2. **Sign everything**: both exes, the installer, and the uninstaller. Timestamp every signature,
   or they expire with the certificate.
3. **Pre-submit to Microsoft.** Submit the signed binaries to the Defender analyst portal as a
   false-positive/developer submission *before* announcing anything.
4. **Test on a clean VM** with Defender and SmartScreen at defaults, from a browser download, with
   Mark-of-the-Web intact. Not on the dev machine, where every exclusion already exists.
5. **Say what the app does, in the README, in the first screen of the installer.** A tool that
   reads every keystroke to detect its hotkey and types into other applications should explain
   itself before the user's antivirus does.

---

## 5. Documentation

Reading the request's "Redmi" as **README** — a document that teaches a stranger to use the app.
Today's `README.md` is a design summary written for the author: it opens with the authoritative
design document, three architectural commitments and a protocol reference. Excellent, and not
what someone who just installed something needs.

* **`README.md`, restructured:** what it is and what it does to your machine → install → first
  run → choosing an engine → the hotkey and the pill → what to do when it doesn't work. Design
  content moves down under a "How it works" heading that links `plan.md`, or out entirely.
* **`docs/setup.md`, new:** connecting to an OpenAI-compatible transcription server — a
  `whisper.cpp` server, a vLLM host, an LM Studio endpoint, the OpenAI API itself — with a worked
  example and the exact URL shape (`/v1/audio/transcriptions`). Plus the Windows AI Speech
  requirements (Windows 11 24H2 / build 26100+, the optional speech model).
* **`docs/supported-targets.md` exists** and is genuinely useful to users — link it from the
  README as "where this works and where it doesn't", which is the honest framing anyway.
* **`LICENSE` — there is none, and this blocks a public release.** Pick one before the first
  binary leaves the machine.
* **A privacy statement, and it has to be exact.** The README currently promises "No cloud, no
  telemetry, no network access during dictation." With the HTTP engine as the *default shipped
  path*, the third clause is no longer true of a default install, and pointing it at
  `api.openai.com` sends the user's microphone to a third party. `models.json` already documents
  this tension against itself rather than hiding it; the shipped copy must do the same, in the
  README and next to the URL field in the settings dialog.

---

## 6. Phases and gates

Each phase ends on a gate that is a demonstration, not an opinion — the shape `plan.md` §7 uses.

| Phase | Work | Gate |
|---|---|---|
| **R0** — decide | Licence chosen. Certificate route chosen and started (lead time is weeks, so this is first, not last). VAD approach measured (§1.4). | A signed hello-world exe downloads and runs on a clean VM without a SmartScreen block. |
| **R1** — native sidecar | `kvit-asrd.exe`: protocol, routing, stability, VAD, HTTP backend. | A full dictation session, `kvit-dictate.exe` ↔ `kvit-asrd.exe`, no Python on `PATH`, against a `whisper.cpp` server on localhost. Existing 479 tests still green, plus the sidecar's own. |
| **R2** — configuration | schema 2, `EngineConfig`, DPAPI key storage, Engine settings page with a working Test button, first-run flow. | A person who has never seen the repo installs, types a URL, presses Test, and dictates. Observed, not asserted. |
| **R3** — package | Inno Setup script, `/MT`, manifest, icon, version resource, signing of all four artefacts, Defender submission. | Clean-VM install → dictate → uninstall, with Defender and SmartScreen at defaults. No warnings, no leftovers. |
| **R4** — document + beta | README rewrite, `docs/setup.md`, privacy statement, LICENSE. Three to five outside testers. | Every tester reaches working dictation using only the README. Each failure becomes a README fix or a bug, and the phase repeats until nobody needs to ask the author. |
| **R5** — release | Tag, publish, download page with the version-check endpoint. | — |

R0 in parallel with R1; the certificate's lead time is the long pole and nothing else depends on
it until R3.

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| Defender or SmartScreen quarantines a signed build anyway | R0's gate is exactly this test; pre-submit to Microsoft; EV if OV reputation does not accumulate |
| Native VAD endpoints worse than Silero, and it reads as "the app is broken" | Measure in R0 against real speech before committing; keep the server-side gate for the HTTP engine; `onnxruntime` in the sidecar is a known upgrade path |
| A batch HTTP model re-punctuates and trips the committed-prefix invariant | Already observed 2026-08-18 and understood; v1 defaults to finalize-only + end-of-session; live preview is opt-in and labelled |
| The 14,966-line port is bigger than it looks | Only ~5,700 lines are in scope, the client half of the protocol already exists, and the Python sidecar stays runnable throughout — R1 can be abandoned without losing anything |
| Users point the engine at a cloud API and leak audio without noticing | Explicit consent text beside the URL field, and a README that says it plainly |
| Windows App SDK experimental channel breaks the on-device engine | It is the second option, not the supported path; detect and grey out rather than fail |
| MSIX turns out to be needed for a Store listing later | Not v1; the `/MT` + manifest work in R3 is a prerequisite either way |

---

## 8. Open questions

1. **Licence** — permissive (MIT/Apache-2.0) or copyleft? Blocks R0.
2. **Certificate route** — is there an organisation with 3+ years of history behind this, which
   decides Azure Trusted Signing vs. an OV token?
3. **Does v1 ship the on-device Windows AI Speech engine** (§1.5), or is "bring your own server"
   the whole v1 story? This is the largest single swing in R1's size.
4. **Who is the user?** "People who run a local ASR server" and "people who want dictation that
   works" want different first-run flows. The plan above assumes the first and makes the second
   possible; if it is the second, the on-device engine stops being optional.
