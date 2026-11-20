# kvit-dictate

Press **Win+Y**, talk, and text appears in whatever field has focus. A replacement for Windows
`Win+H` that types the words you actually use — `llama.cpp`, `Qwen3.6`, `sm_120`, file paths, CLI
flags — and lets you decide how long a pause ends a sentence.

Windows 10 (1809+) and 11, x64. Dictating with no server needs Windows 11 24H2 or later.

> **Two ways to turn speech into text, and you pick one in Settings → Server:**
>
> * **This PC** — Microsoft's own on-device speech model (Whisper Small, an optional Windows
>   component). Nothing leaves the computer and there is no server to run. Windows 11 24H2 or
>   later; the model downloads once, through Windows Update, when you press **Test**. It is
>   slower than a GPU box — about 3 s for a 5 s sentence on a fast desktop CPU — and the Windows
>   API it uses is still marked *experimental* by Microsoft.
> * **A speech service** you run or rent — anything that serves `/v1/audio/transcriptions`.
>   Faster and better on technical vocabulary, and your audio crosses the network to it.
>   [`docs/setup.md`](docs/setup.md) is how to get one.

---

## What it does to your machine

Two of these look, to an antivirus scanner, exactly like the things malware does. You should hear
it from the program before you hear it from Defender.

* **It reads every keystroke.** A global low-level keyboard hook is how the hotkey works in
  whatever application you are typing into. That hook sees every key you press, everywhere. It is
  used to recognise one chord, and — while you are dictating — to let you edit the pill's text
  before it is typed in. Keystrokes are never recorded, never stored and never sent anywhere.
* **It types into other applications.** That is the entire product: it synthesises keystrokes into
  the window that has focus. It refuses to do so into password fields, into windows running with
  higher privileges than it has, and into applications you tell it to leave alone.
* **It opens your microphone while you hold the hotkey**, and sends that audio to the speech
  model you chose — on this PC, or the server you configured. Nothing is recorded before you
  press the hotkey, and nothing is sent anywhere else.

The source is public and this build is not code-signed. Decide accordingly.

## Privacy, exactly

No cloud service. No telemetry. No account. No automatic updates and nothing downloaded at run
time. Recognised text is never written to a log unless you switch that on, and that switch is
compiled out of release builds entirely.

**With This PC, your audio never leaves the computer.** The recogniser runs in a helper process
on this machine (`kvit-speech\kvit-speech.exe`), and the only network use is the one-time model
download Windows does when you press Test.

**With a speech service, your audio does cross the network**, to the server you named and
nowhere else — unencrypted, and to a server that does not ask who you are. Run it on a network
you control. [`docs/protocol.md`](docs/protocol.md) §2.8 is the long version, and `wss://` works
if you put a proxy with a certificate in front of your server.

---

## Install

Download the installer or the portable ZIP from the [releases page](https://github.com/kvit-s/kvit-dictate/releases).

* **Installer** — per-user, into `%LOCALAPPDATA%\Programs\kvit-dictate`, no administrator prompt.
  That is deliberate: the first thing this program does is register a global keyboard hook, and
  an elevation prompt on top of that is the wrong way to ask for trust.
* **Portable ZIP** — unpack it anywhere and run `kvit-dictate.exe`. One executable; nothing else
  is needed.

**SmartScreen will warn you.** The binary is unsigned, and an unsigned program that hooks the
keyboard and synthesises input is exactly what SmartScreen exists to warn about. If you install
it anyway, that is a decision you are making on purpose rather than a step you are clicking past.

Nothing is written outside your own user profile:

```
the program   %LOCALAPPDATA%\Programs\kvit-dictate
settings      %LOCALAPPDATA%\kvit-dictate\settings.json
logs          %LOCALAPPDATA%\kvit-dictate\logs
```

## First run

1. It starts in the tray and opens **Settings → Server**, because it cannot do anything until
   you say what turns speech into text.
2. **No server?** Choose **This PC: Windows speech, no server** and press **Test**. If Windows
   has not installed its speech model yet, Test asks Windows for it (a few hundred MB, once) and
   waits; then it reports what answered. Press OK and you are done. Windows speech has no live
   preview: the pill says *Listening* while you talk, and each sentence appears about a second
   and a half after you pause.
3. **Have a speech service?** Type its address — anything that serves
   `POST /v1/audio/transcriptions`, for example `http://your-gpu-box.lan:10301` — and press
   **Test**. It connects for real and tells you what answered: the model, the mode and the
   round-trip time. For a hosted service that wants a key, such as OpenRouter, put the key in
   **API key** (it is stored encrypted). What that costs in privacy and features is in
   [`docs/setup.md`](docs/setup.md). Press OK.

That is the whole setup. **There is nothing else to start and no Python to install.**
kvit-dictate ships with `kvit-asrd.exe`, a translator that speaks its own protocol on one side
and, on the other, `/v1/audio/transcriptions` or the Windows speech model; the app launches it,
supervises it, and stops it when you quit. Test starts a private copy and completes the real handshake with it, so a green
Test means the whole path answered — the app, the translator and your service.

Already run a **kvit-dictate protocol server**? Pick "I run my own kvit-dictate server" and give
it the `ws://` address. Nothing is launched for you in that mode.

Do not have either? [`docs/setup.md`](docs/setup.md) §1 gets you a working endpoint in five
minutes that proves the plumbing without needing a model, and §2 gets you a real one.

Until you have chosen This PC or a server, **the hotkey does not open your microphone**, the pill does not
appear, and the tray says why. A dictation tool that silently recorded into nothing would be
worse than one that refuses.

## Dictating

**Press Win+Y.** A small pill appears near your caret and fills with what you are saying.
Text is typed into your document as it becomes stable — once a word is in, it is never taken back
and never rewritten.

* **Press Win+Y again** to finish. The last of the sentence is typed in.
* **Press Esc** to throw the session away. Nothing already typed is removed; nothing further is
  added.
* **Stop talking** and the session ends on its own after a pause you control (Settings →
  Dictation). There are two separate timers — one ends a sentence, one ends the whole session —
  and conflating them is the single most common misreading of this UI.
* The tray icon says whether the microphone is open, in words and in shape: the microphone glyph
  is filled when it is open and hollow when it is not, so it does not depend on telling two
  colours apart.

The hotkey is configurable (Win+J, Win+O, F13–F24, right Ctrl, CapsLock and others) in
Settings → Dictation.

### Push to talk

Tick **Push to talk** in Settings → Dictation and the hotkey stops toggling: the microphone is
open only while you hold the key, and letting go finishes the session and types the text into
your document. Transcription still runs while you talk — releasing the key is what sends the
result to the application, not what starts the work.

Releasing any part of the chord counts as letting go, so a binding you can comfortably hold with
one finger — F13–F24, right Ctrl, right Alt or CapsLock — suits this better than a Win-key
combination. Everything else is unchanged: Esc still throws the session away, and clicking
somewhere else still ends it.

## When it does not work

| What you see | What it means |
|---|---|
| Tray says *no server configured* | Settings → Server. Nothing works until this is set. |
| Tray says *the server is not answering; retrying in N s* | The server box is off, or the address changed. It keeps trying, backing off; it does not hammer. |
| **Test** says `connect_timeout` | Nothing answered: wrong address, machine off, or a firewall. |
| **Test** says `connect_refused` | Something answered and said no: the server is not running, or not on that port. |
| **Test** says *answered HTTP 200 instead of upgrading* | The URL points at an HTTP API, not the WebSocket endpoint. |
| **Test** says `server_busy` | Someone else is using that server. |
| Tray says *this window cannot be dictated into* | A password field, an elevated window, or an application listed as unsupported. [`docs/supported-targets.md`](docs/supported-targets.md) says where this works and where it does not. |
| Text lands in the wrong place | The caret anchor was wrong. Settings → Overlay, and see `plan.md` §1.5, which is honest about where anchor estimation fails. |
| Nothing at all happens on the hotkey | Another program owns that chord. Pick a different one in Settings → Dictation; F13–F24 are almost always free. |

Logs are at `%LOCALAPPDATA%\kvit-dictate\logs` and contain no recognised text unless you turned
that on. `kd_probe --server <url>` runs the same check the Test button does, from a command line,
and its output is the useful thing to attach to a bug report.

---

## How it works

The authoritative design document is [`plan.md`](plan.md); the narrower "what shipping requires"
document is [`release-plan.md`](release-plan.md). Where either disagrees with this README, it
wins.

### Two halves and a published contract

```
┌─────────────────────────────┐            ┌──────────────────────────────┐
│  kvit-dictate.exe (C++)     │            │  any conforming ASR server   │
│                             │  WebSocket │                              │
│  • global hotkey            │  ws:// or  │  • VAD / endpointing         │
│  • WASAPI mic capture       │  wss://    │  • streaming decode          │
│  • session state machine    │◄──────────►│  • stability computation     │
│  • anchor estimation        │  binary    │  • partial/final emission    │
│  • commit engine            │  frames    │                              │
│  • overlay / injection      │  + JSON    │  (yours, or server/ here)    │
│  • tray + settings          │            │                              │
└─────────────────────────────┘            └──────────────────────────────┘
```

Almost nobody runs one of those, and everybody's GPU box already speaks
`/v1/audio/transcriptions`. So the installer ships the right-hand box too:

```
┌──────────────────────┐         ┌──────────────────────┐        ┌────────────────────┐
│  kvit-dictate.exe    │   ws    │   kvit-asrd.exe      │  HTTP  │  your ASR service  │
│                      │ ──────► │                      │ ─────► │                    │
│  hotkey, capture,    │127.0.0.1│  protocol server     │        │  /v1/audio/        │
│  session, commit,    │ ◄────── │  endpointing (VAD)   │ ◄───── │    transcriptions  │
│  injection, overlay  │  frames │  stability, WAV      │  JSON  │                    │
└──────────────────────┘         └──────────────────────┘        └────────────────────┘
      ~1.2 MB                       headless, no Python                yours
```

`kvit-asrd.exe` is built from this repository, needs no Python and no runtime, and is launched
and supervised by the client on a loopback port it chooses. It is a *translator*, not a
recogniser: it still owns no ASR. The protocol between the two halves is unchanged — the
translator is a conforming server like any other, which is the test of whether the boundary was
drawn in the right place, and it passes.

[`docs/protocol.md`](docs/protocol.md) is the contract, it is licensed Apache-2.0, and it is
written to be implementable by someone who has never spoken to its author. §15 is a complete
worked example of a conforming server in about 400 lines of Python standard library, and that
example is executed by the test suite rather than merely printed.

The client owns capture, the hotkey, the session machine, the commit engine, injection, the
anchor and the overlay. It owns **no ASR of any kind**. Recognition, endpointing and stability all
live behind the endpoint you configure.

### Three commitments that shape everything else

1. **Committed text is never repaired.** Every byte typed into your document must remain a prefix
   of every later hypothesis. Streaming ASR revises what it already said; `Win+H` can hide that
   because it owns its own text box, and this cannot, because it is typing into someone else's
   document. Replaying a revision means sending backspaces into an arbitrary application, where
   backspace can navigate a browser, dismiss an autocomplete, or eat a terminal prompt. So
   revisions are absorbed in the overlay *before* commit, and a hypothesis that contradicts
   committed text is logged and counted, never applied. (`plan.md` §1.4, `docs/protocol.md` §8)
2. **A late message can never reach the wrong window.** Every message carries a `session_id`, and
   anything naming a session that is not the current one is dropped — not clamped, not queued.
   (`docs/protocol.md` §7)
3. **Nothing is persisted and nothing is sent that you did not ask for.** No transcripts or audio
   by default, no telemetry, no cloud fallback, no model download at run time. (`plan.md` §1.9)

### Source layout

```
src/
  main.cpp            tray, message loop, lifecycle          (the single-threaded serialisation point)
  translator.{h,cpp}  launching and supervising kvit-asrd    ← job object, fixed image path
  asrd/               kvit-asrd.exe: the translator          ← RFC 6455 server, VAD, stability, HTTP
  protocol.{h,cpp}    framing + message codec                ← pure, unit-tested, no Windows
  session.{h,cpp}     session/utterance state machine        ← pure, unit-tested
  commit_engine.{h,cpp} stability → injection ops            ← pure, unit-tested
  ipc_client.{h,cpp}  WebSocket + named-pipe transports, framed I/O, bounded queues
  audio_capture.{h,cpp} WASAPI → 16 kHz mono s16             (AudioConverter half is pure and tested)
  anchor.{h,cpp}      3-source confidence-scored anchor      (diff_bitmaps/score_diff are pure)
  injector.{h,cpp}    SendInput / clipboard; UIPI + focus guards
  overlay.{h,cpp}     layered, click-through, capture-excluded
  hotkey.{h,cpp}      WH_KEYBOARD_LL + WH_MOUSE_LL           (InputFilter is pure and tested)
  settings.{h,cpp}    settings, the server address, the models.json manifest
  util/               utf8, json, log, result, clock         ← all pure
docs/examples/        a complete conforming server, in the standard library
packaging/windows/    icon generator, manifest, .rc, Inno Setup script, release build
tools/probe/          Phase 0B measurement harness; also `--server <url>`, the Test button's twin
tools/kvit-speech/    C# helper for the on-device Windows AI speech model (WinRT); shipped as kvit-speech\
tools/testtarget/     controlled target: edit, richedit, multiline, password, RTL
tests/                kd_test harness + suites + shared protocol fixtures
server/kvit_asr/      the reference server: ws, session, stability, vad, backends/
bench/                corpus recorder, experiment matrix, metrics
```

**A convention worth knowing before reading any header:** no header in `src/` includes
`<windows.h>`. Windows handles cross module boundaries as `uintptr_t`, and geometry as the plain
structs in `anchor.h`. This is not portability theatre — the client is Windows-only and always
will be — it is what keeps the pure-logic modules compilable and testable without dragging the
Win32 surface into every test translation unit.

### Why there is no macOS version

The client is not portable and porting it is not a packaging task: `src/` is `WH_KEYBOARD_LL`,
WASAPI, `SendInput` with UIPI integrity comparison, `BitBlt` caret hunting, DirectWrite, layered
windows, `Shell_NotifyIcon`, DPAPI and WinHTTP. On macOS every one of those is a different API,
and the two hardest — global hotkeys and synthetic keystrokes into another app — sit behind
Accessibility and Input Monitoring permissions that change the shape of the product. A Mac
version is **a second client against the same protocol**, which is a real and attractive project
and is not this one. The published protocol is what makes it possible without asking anybody.

---

## Building

Visual Studio 2022 with the C++ toolchain, and CMake ≥ 3.24. There are **no third-party C++
dependencies** — no Qt, no nlohmann/json, no Catch2, no fmt. If a build step ever wants to
download something, that is a bug.

```bat
win-build.bat                 rem configure, build, test (Debug)
win-build.bat Release
win-build.bat Debug clean
```

The reference server needs CPython 3.12 or newer and, for the `mock` backend, nothing else at
all — the whole client can be exercised on a bare interpreter.

```
py -3.12 -m unittest discover -s server/tests -t .
```

A release build, staged and packaged:

```powershell
packaging\windows\build-release.ps1
```

It refuses to package a red build, and it checks the executable for dynamic-CRT imports —
because a silent revert from `/MT` produces an installer that works on the build machine and
fails on a clean one.

A published release runs the same script on a GitHub Windows machine. Pushing a tag `v<version>`
that matches `CMakeLists.txt` (`v1.1.0`, or `v1.1.0-rc1` for a pre-release) makes
`.github/workflows/release.yml` build, test and attach the installer, the ZIP and
`SHA256SUMS.txt` to a new release. Run that workflow by hand from the Actions tab to build without
publishing anything.

### Cross-language conformance fixtures

`tests/fixtures/protocol/` holds 20 cases read by **both** the C++ suite and the Python suite.
Neither may keep a private copy: the point is that a disagreement between the two implementations
shows up as a failing shared fixture rather than as garbled text in someone's document
(`docs/protocol.md` §12).

---

## Status

Honest, and specific about what has and has not been run.

| Area | State |
|---|---|
| **Protocol** | Published, Apache-2.0, two transport bindings over one framing layer. Concurrency and model discovery answered (§2.7, §5.1). 20 shared fixtures run by both endpoints, plus a worked example server that the suite executes. |
| **Client** | Every module under `src/` implemented and wired together. 494 tests. The whole path — hotkey, microphone, session, commit, injection, finalisation — has been executed against a real target. |
| **Transport** | WebSocket, through WinHTTP, verified end to end against a server written only from the specification. The named-pipe binding survives for the bench. |
| **Reference server** | Implemented: `ws`, session, stability, VAD, and backends for mock, sherpa-onnx, zipformer, whisper, faster-whisper, openai-http and windows-speech. 475 tests. Two have run against real audio. |
| **Configuration** | Server address, optional model, a Test button that opens a real connection, and a no-server state with words in it. |
| **Packaging** | One 1.16 MB executable, static CRT, icon, version block, application manifest. Both artefacts build: the portable ZIP and the per-user installer. Neither has been *installed* anywhere yet. |
| **Signing** | ❌ **Not done, and the largest risk in the release.** No certificate exists. An unsigned binary that hooks the keyboard earns a SmartScreen warning at best. |
| **Clean-VM test** | ❌ Not done. The gate is a browser download with Mark-of-the-Web intact, Defender and SmartScreen at defaults. |
| **Phase 0B measurements** | ⚠️ 11 of 162 cells, all against `tools/testtarget`. Until an operator runs the rest, `docs/supported-targets.md` is a template rather than evidence. |
| **No-server dictation** | ✅ **This PC** ships in the installer: `kvit-asrd --backend windows-speech` drives `kvit-speech.exe`, published self-contained (.NET and Windows App SDK runtimes inside, ≈ 230 MB installed, 62 MB installer). Verified 2026-11-19 on Windows 11 build 26300, CPU only: real speech through the real client, translator and model, transcribed word for word. The Settings page itself has not been looked at on screen, and no clean machine has run it. |
| **Real-model dictation** | ⚠️ Runs against `openai-http` and `windows-speech`. The four backends that need a model on disk and an environment that does not exist yet still have not. |
| **Text normalizer** | ⚠️ Implemented, off by default, never used in real dictation. The first measurement caught the model rewriting `llama.cpp` as `LLMCpp`; the guard rejects it. |

---

## Licensing

Two licences, deliberately:

| Path | Licence |
|---|---|
| `src/`, `tests/`, `tools/` — the Windows client | **MPL-2.0** ([`LICENSE`](LICENSE)) |
| `docs/protocol.md`, `server/`, `bench/` — the protocol and the reference server | **Apache-2.0** ([`LICENSE-APACHE`](LICENSE-APACHE)) |

Copyleft where the value is, permissive where the adoption is: the point of publishing the
protocol is to get other people writing servers, and a spec nobody can read without first reading
a licence does not spread.

Contributions are welcome and there is a contributor agreement — including a plain statement of
why it asks for more than the outbound licence does. See [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/cla.md`](docs/cla.md).

## Reading order

1. [`docs/setup.md`](docs/setup.md) — if you just want it working.
2. [`plan.md`](plan.md) — the design, and the reasoning behind each decision that could have gone
   another way. §1.4 (the commit invariant) and §1.5 (anchor estimation, including an honest list
   of where it fails) are the two that most affect how the code reads.
3. [`release-plan.md`](release-plan.md) — what shipping to other people required, and what was
   deliberately left out.
4. [`docs/protocol.md`](docs/protocol.md) — normative for both endpoints. §15 if you are writing a
   server.
5. [`bench/CANDIDATES.md`](bench/CANDIDATES.md) — which models are real, what they need, and why
   the shortlist is four entries.
6. `src/*.h` — each header states its responsibility, the `plan.md` section it implements, and its
   **threading contract**. Those contracts are not boilerplate: the keyboard hook must never block
   or Windows silently unhooks it, the WASAPI callback runs under a latency budget, and the pure
   state machines are single-threaded on purpose.
