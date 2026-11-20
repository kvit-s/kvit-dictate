# kvit-dictate — Release Plan (v1.0, first build for other people)

Status: **mostly built, and §1 is superseded.** Revision 2, 2026-08-27, against commit
`59b0a57`; implemented 2026-08-28. §10 is the honest account of what was built, what was not,
and what cannot be verified from a keyboard.

**Read `openai-translator.md` before acting on §1 or §2.2 of this file.** Revision 2's central
move — ship the client, treat the server as configuration the user supplies — was implemented
faithfully and turns out to be wrong for the endpoint this project actually talks to. The user's
service speaks OpenAI-compatible HTTP, not this protocol, so a translator has to exist
somewhere; deleting the shipped one did not remove it, it removed the *supervision* of it and
left a Python window the user has to keep open. Revision 1's `kvit-asrd.exe` was right and
`openai-translator.md` is revision 3 of that decision. The protocol boundary itself stands
unchanged.

`plan.md` is the design document for the experimental phase and it still wins on every question
of architecture *inside* the client. This document is narrower and later: it covers only what has
to change for someone who is not the author to install this and use it. It deliberately reverses
one line of `plan.md` §6, which lists "installer/auto-update" among the non-goals. That was the
right call for the experiment; it is the wrong call now, and reversing it is the point of this
file.

**Revision 2 replaces revision 1's central decision.** Revision 1 proposed porting the Python
sidecar to a native `kvit-asrd.exe` so the app could ship self-contained. That is no longer the
plan. The release ships **the client only**; the app requires an ASR server, which is
configuration, and the server is not part of the release at all — not even a local one. Revision
1's §1.3 port table and §1.4 VAD question are both obsolete and are gone. What replaced them is
§1 below, and it is a great deal smaller.

---

## 0. Scope

**In:** Windows 10 (1809+) and 11, x64. One installer, a settings page where a user points the
app at a server, no Python on the user's machine, `docs/protocol.md` promoted to a public API
specification, and a README that teaches a stranger to use the thing.

**Out:**

* **The server. Any server.** Including a local one. The app requires an endpoint the user
  provides; the release does not ship, bundle, install or supervise one. A reference server is a
  separate deliverable on its own timeline (§1.5).
* **On-device / offline dictation.** This is the direct cost of the line above, stated plainly
  rather than discovered later: with no shipped server there is no offline path, so the Windows
  AI Speech work in `tools/kvit-speech` is not in v1 and the app does nothing useful on a machine
  with no server reachable. Accepted deliberately.
* **macOS.** It came up and deserves a straight answer rather than a placeholder: the client is
  not portable and porting it is not a packaging task. `src/` is `WH_KEYBOARD_LL` (hotkey.cpp),
  WASAPI (audio_capture.cpp), `SendInput` plus UIPI integrity comparison (injector.cpp), UI
  Automation and `BitBlt`/WGC caret hunting (anchor.cpp), DirectWrite and layered windows
  (overlay.cpp), `Shell_NotifyIcon` (tray_icon.cpp), DPAPI, WinHTTP and named pipes with
  logon-SID DACLs (ipc_client.cpp). `CMakeLists.txt:20` refuses to configure off Windows and says
  why. On macOS every one of those is a different API, and the two hardest — global hotkeys and
  synthetic keystrokes into another app — sit behind Accessibility and Input Monitoring
  permissions that change the shape of the product. A Mac version is a **second client against
  the same protocol**, which is a real and attractive project and is not this one. Everything in
  §1 makes it easier: after this release the protocol is a documented network API rather than a
  private pipe to a child process.
* **Auto-update.** A version check that points at a download page, nothing more. A silent updater
  is a second signed-binary supply chain and it can wait. *Since 1.0.0-rc2 (src/updater.h): the
  check runs daily and, when the user presses Install, downloads the release's own installer,
  checks it against the release's SHA256SUMS.txt and runs it. Still never silent, still unsigned,
  and the portable ZIP only gets the download page.*
* **Remote / internet servers.** LAN and loopback only (§2.4).

---

## 1. The architecture: the app requires a server

### 1.1 The decision

The client keeps everything it is good at — capture, hotkey, the session machine, the commit
engine, injection, the anchor, the overlay — and owns **no ASR of any kind**. Recognition,
endpointing and stability all live behind a network endpoint the user configures.
`docs/protocol.md` becomes the contract between them, and the contract becomes public.

This is smaller than it sounds, because **the split already exists**. `server/` already owns VAD,
stability and the backends; `src/` already owns capture, commit and injection; and
`docs/protocol.md` already specifies the boundary in far more detail than most projects manage.
The only thing that is local-only is the *transport*.

### 1.2 What actually changes

| Component | Change |
|---|---|
| `docs/protocol.md` §3–§10 (framing, encoding, handshake, message catalogue, ordering, text semantics, errors) | **None.** These are transport-agnostic already: a 4-byte length, a type byte, a payload, ≤ 65541 bytes on the wire, with a reference decoder (§3.4) that already handles partial reads. |
| `docs/protocol.md` §2 (Transport) and §2.4 (child launch and peer validation) | **Replaced** by a network binding (§1.3). This is the only part of the spec that is about a pipe and a child process. |
| `src/ipc_client.cpp` transport layer | **Replaced.** `CreateFile`/`ReadFile`/`WriteFile` on a pipe → Winsock or WinHTTP. The framing code and its fixtures are untouched. |
| `src/ipc_client.cpp` lifecycle — `CreateProcessW` (:1291), the job object, `--parent-pid` liveness | **Deleted from shipped builds**, and that is still true of *this* path. **But see the note below §1.2's table: a shipped build spawns a child again, a different one.** |
| `session.cpp` — `Effect::KillChild`, `Effect::RespawnChild` | **Re-meaning, not deletion.** "Kill the child" becomes "close the connection", "respawn" becomes "reconnect with backoff". The states and their tests survive; `App::on_sidecar_lost` (`main.cpp:2195`) already does the right thing when the engine vanishes. |
| `models.json` and its whole spawn machinery | **Gone from shipped builds**, kept for the bench. Its job was mapping a model id to an interpreter and argv; with no child process there is nothing to map. Server URL replaces it (§2). |

> **Superseded in part, 2026-08-28 — `openai-translator.md`.** The two rows above said "there is
> no child to spawn". There is one again, and the reason is that revision 2 of §1 assumed the
> user's endpoint speaks this protocol, and the endpoint on the author's own network does not: it
> is an OpenAI-compatible `/v1/audio/transcriptions` service. Under revision 2 a translator still
> existed, still ran on the user's machine, and was started by hand from a `.bat` file — the
> moving parts did not go away, only the supervision of them did.
>
> So `kvit-dictate.exe` now launches `kvit-asrd.exe`, a second native executable built from this
> repository and installed beside it. **What stays deleted is what §1.2 was actually about:** the
> named-pipe path that spawns an arbitrary interpreter named in a user-editable `models.json`,
> with a user-supplied argv and environment. `KD_ENABLE_SIDECAR_SPAWN` is still OFF in shipped
> builds and the `#ifdef` around `ChildSpec` and `IpcClient::start` has not moved.
>
> What is added back launches **one fixed executable, by absolute path, from the installation
> directory** — `GetModuleFileName(nullptr)`'s directory plus `kvit-asrd.exe`, with no setting,
> environment variable or manifest able to redirect it (`src/translator.h`). The release gate
> changed with it; see the note on §1.2's row in the summary table below.
| `server/kvit_asr/transport.py` (800 lines of ctypes against kernel32) | **Replaced by a socket**, which is stdlib and about a tenth of the size — and which makes the reference server cross-platform for free. |
| Everything else in `server/` — `vad.py`, `stability.py`, `session.py`, the backends | **Unchanged, and out of scope for this release.** |

Nothing about the committed-prefix invariant, `stable_bytes` handling, `session_id` discipline or
the violation path changes. The 479 tests stay green; the framing fixtures (004–011) keep testing
the same bytes.

### 1.3 The network binding: WebSocket

**Recommended: one protocol frame per binary WebSocket message, over `ws://` or `wss://`.**

* **WinHTTP already does WebSocket** (`WinHttpWebSocketCompleteUpgrade` / `Send` / `Receive`,
  Windows 8+), and `src/normalizer.cpp` already talks to WinHTTP. So this adds **no third-party
  dependency**, which matters because `CMakeLists.txt:1-9` forbids them outright and means it —
  there is no `find_package()` in the file. Hand-rolling RFC 6455 would have been the alternative
  and it is several hundred lines of masking and fragmentation nobody should write again.
* **TLS and proxies come free** with the same API, which is what makes §2.4's "accept `https://`
  if someone types it" cost nothing.
* **The ecosystem speaks it.** Every ASR server anyone might put behind this already has a
  WebSocket story; a bespoke TCP protocol would need one written for it each time.
* The existing 4-byte length prefix is *redundant* inside a WebSocket message, which has its own
  boundaries. **Keep it anyway.** It costs five bytes per frame, and it means one decoder, one
  fixture corpus and one spec section serve both bindings — including the named-pipe one that
  stays alive for the bench.

Raw TCP was the alternative and is genuinely smaller (the framing already handles stream
reassembly, so it is Winsock plus the existing decoder). It loses TLS, proxy traversal and every
existing server's front door. WebSocket wins on everything except a few hundred lines.

One role reversal to note in the spec rewrite: today the **client** creates the pipe and the
sidecar connects to it. Over a network the **server** listens and the client connects. The
application-level roles in §1 of the protocol are unaffected — the client still sends `start` and
audio, the server still sends `partial`/`final` — but §2's language about who creates what has to
be rewritten, not adapted.

### 1.4 Two spec gaps the network binding creates

Both are consequences of the endpoint no longer being a private child process, and both need
answering in the spec rewrite rather than discovering in the field:

1. **Concurrency.** §7.2 says one session at a time, which was trivially true when the server was
   a child of exactly one client. A LAN server may now have three people pointed at it. The spec
   must say whether a server accepts concurrent connections and what a client does when refused —
   a defined error is fine, silence is not.
2. **Model discovery.** `ready` (§6.2) reports *one* model — `model`, `mode`, `chunk_ms`,
   `context_supported` — because "a model change means a new child process". A server hosting
   several models has no way to advertise them, so the settings page cannot offer a list. Either
   add an optional `models[]` to `hello_ack`, or accept that the model is a free-text field the
   user types. The first is a small addition and makes the settings page much better; the second
   is v1-acceptable.

The good news is that `ready` already carries enough for the client to **adapt its UI to the
server it found**: `mode` says whether to expect live preview or one commit at the endpoint, and
`context_supported` says whether the vocabulary list is worth showing. That is exactly the
capability negotiation a public API wants, and it is already specified.

### 1.5 The reference server, later

Not in this release. When it happens it is a separate repository or at least a separate release
train, permissively licensed (§5), and its job is to make the first-run cliff (§8) survivable:
one download, one `run.bat`, a working endpoint. Until then the README points at the servers that
already implement an OpenAI-compatible transcription API and explains how to put one behind the
protocol.

### 1.6 Correction: what the VAD is actually doing today

Revision 1 posed a choice between Silero-over-ONNX and an energy VAD. That choice was already
made, by default, months ago and the plan misread it. `config.py:248` defaults `--vad auto`,
`--silero-model` defaults to `None`, no `models.json` entry sets it, and `SileroOnnxScorer`
imports `onnxruntime` inside `load()` — so on every run to date it fails to load and `auto` falls
back silently to `EnergyZcrScorer`, which is stdlib and, in its own file's words, "not a
substitute for Silero". **That is what has been endpointing every dictation.** The separate
`vad_gate=true` in `models.json` is a different mechanism: a call to the *server's* Silero over
`/v1/audio/vad`.

Under this revision the whole question leaves the client's scope — endpointing is the server's
job and the server is out of scope. It is recorded here only because the reference server will
inherit it, and because a plan that quietly dropped a risk it had raised would be worse than one
that says where it went.

---

## 2. What a user configures

### 2.1 The gap

There is no way for a user to type a server address. The endpoint is a Python module constant
(`server/kvit_asr/backends/openai_http.py:95` — `http://strixhalo.lan:10301`), the model choice
is a `models.json` id, and `Settings` (`src/settings.h:157`) has twenty-odd fields and not one of
them is an address. Everything about the current configuration assumes the person running it also
wrote it.

### 2.2 Work

* **`ServerConfig` on `Settings`**: `url`, `model` (optional), `connect_timeout_ms`,
  `reconnect_backoff`. Persisted with the rest.
* **A "Server" page in the settings dialog** (`src/settings.cpp:1517+`): the URL, an optional
  model, and a **Test** button that opens a real connection, completes the handshake, and reports
  what came back — the server build id from `hello_ack`, the model and `mode` from `ready`, and
  the round-trip time. The normalizer's on-demand WinHTTP test is the precedent and it answers in
  295 ms. A settings page that can prove the endpoint works converts most of the future support
  burden into one button.
* **`models.json` stops being user-facing.** Keep the file and the parser for the bench; the
  shipped installer does not lay it down and the dialog does not read it.
* **First run.** With no `settings.json` the current default comes from `models.json`
  `default_model`, which is `"mock"` — a scripted fixture that types invented sentences. A shipped
  build must never start there. First run opens the Server page with an empty URL and a short
  explanation, and the tray says "no server configured" rather than pretending to listen.
* **Defaults that respect the invariant.** `models.json` records that the HTTP path produced a
  **committed-prefix violation within seconds** of its first human run (2026-08-18) — structurally,
  because a batch model re-punctuates earlier words as later context arrives. v1 therefore ships
  finalize-only behaviour and end-of-session cadence; live preview stays available and is labelled
  for what it is.

### 2.3 What the client does when there is no server

This is now a first-class state, not an error path, because it is what every fresh install looks
like and what every user sees when their server box is off. Requirements: the hotkey must not
open the microphone; the tray must say what is wrong in words; the pill must not appear; and the
failure must never be silent. `App::on_sidecar_lost` and the existing `Unsupported`/`Error` tray
states are the machinery — this is mostly wiring and wording.

### 2.4 Security posture, stated on purpose

**LAN and loopback only. Remote servers are explicitly unsupported.**

This is less of a change than it looks: the audio *already* crosses the network in plaintext.
`DEFAULT_BASE_URL` is `http://strixhalo.lan:10301`, so every session run against that endpoint has
been posting unencrypted WAV to another host. The named-pipe DACL only ever protected the local
hop between two processes on the same machine; the network hop was open the whole time. Moving
the control channel onto the network puts it where the audio already was.

So: no auth infrastructure, no certificate management, no token store in v1. Two cheap things
stay in anyway:

1. **Accept `https://`/`wss://` if the user types it.** WinHTTP does TLS through the same calls,
   so this is a matter of not rejecting the scheme. It costs nothing today and it is what makes a
   future remote tier a pricing decision rather than a protocol change.
2. **Make "unsupported" mechanical rather than documentary.** The moment there is a URL box,
   somebody types a public hostname into it, and that person will dictate a password into it
   within a week. If the host is neither loopback nor RFC 1918 and the scheme is unencrypted, the
   dialog shows a warning line. Not a block — a sentence. Roughly ten lines of code.

And one line in the README: *audio crosses your network unencrypted; run this on a network you
control.* The repo already documents its own tradeoffs against itself in `models.json`; the
shipped copy keeps the habit.

---

## 3. The installer

### 3.1 Choice

| Option | Verdict |
|---|---|
| **Inno Setup, per-user** | **Recommended.** Installs to `%LOCALAPPDATA%\Programs\kvit-dictate` with no admin prompt, which matters because the first thing this app does is install a global keyboard hook — an elevation prompt on top of that is precisely the wrong first impression. Free, one `.iss` script, signs its own uninstaller. |
| MSIX | Attractive on paper (clean uninstall, App Installer updates) and needs a spike before anyone commits: the app's whole job is a low-level keyboard hook, synthetic input into arbitrary windows, and UI Automation and `BitBlt` against other processes' windows. Full-trust packaged apps can do this; Store certification would not accept it, and the packaged-identity edge cases here are unmeasured. Not v1. |
| WiX / MSI | The right answer if this is ever deployed by an enterprise over GPO. Per-machine, admin, steepest authoring curve. Defer until asked. |
| Portable ZIP | Ship it alongside. It costs nothing and it is what a suspicious user tries first. |

### 3.2 Contents, and two build changes it forces

Payload is now **one executable**: `kvit-dictate.exe`, plus `LICENSE` and the README. No sidecar,
no `models.json`, no Python, no runtime.

* **The CRT.** The build is `/MD` today and `CMakeLists.txt:96-98` says static linking was left
  alone because *"this is not a redistributable yet"* and would have to be revisited for signing.
  This is that moment. `/MT` on a single executable removes the redistributable step entirely.
* **The application manifest**, still `TODO(phase-3)` at `CMakeLists.txt:243`: PerMonitorV2 DPI
  awareness and `requestedExecutionLevel asInvoker`. The DPI half is a live correctness bug on
  scaled monitors — `anchor.cpp:801` already warns at runtime when it is missing. The `asInvoker`
  half must be explicit, because `plan.md` §1.7 makes elevated targets an *unsupported state* and
  the app must never be the thing that elevates.

Also needed: an application icon (there is none), a version resource, a Start Menu shortcut, an
optional run-at-login checkbox wired to the existing `Settings::start_with_windows`, and an
uninstaller that leaves `%LOCALAPPDATA%\kvit-dictate\settings.json` alone unless the user ticks
"also remove my settings".

---

## 4. Signing and Defender — the largest risk here

`plan.md` §6 already called it: *"Antivirus flags keyboard hook + synthetic input — expected, this
looks like a keylogger to heuristics. Sign early; test under Defender before Phase 4 closes."*
That risk has been carried unmitigated the whole way. An unsigned binary that hooks the keyboard
and synthesises input, downloaded from the internet, earns a SmartScreen warning at best and a
silent quarantine at worst — and a user who gets either never files a bug, they just leave.

1. **Get a certificate.** Since June 2023 code-signing keys must live in certified hardware, so
   the practical options are **Azure Trusted Signing** (subscription, no HSM to manage, requires
   an organisation with three-plus years of verifiable history) or an **OV certificate on a
   hardware token** from a CA. Only **EV** grants SmartScreen reputation immediately; OV
   accumulates it over download volume, which for a niche tool can take a long time.
2. **Sign the executable, the installer and the uninstaller.** Timestamp every signature or they
   expire with the certificate.
3. **Pre-submit to Microsoft** through the Defender developer submission portal, before
   announcing anything.
4. **Test on a clean VM** with Defender and SmartScreen at defaults, downloaded through a browser,
   Mark-of-the-Web intact. Not on the dev machine, where every exclusion already exists.
5. **Explain the app before the antivirus does** — in the README and on the installer's first
   screen. A tool that reads every keystroke to find its hotkey and types into other applications
   should say so itself.

---

## 5. Licensing

**`src/` and the client: MPL 2.0.** Consistent with the author's other projects, which have
already been analysed on this question. File-level copyleft: modifications to these files come
back, and the files can still be combined into a larger work.

**`docs/protocol.md`, `server/`, and the future reference server: Apache-2.0.** A protocol spec
wants to spread without anyone reading a licence first, and the whole point of §1 is to get other
people writing servers. Copyleft where the value is, permissive where the adoption is.

**A contributor agreement, in place before the first outside contribution is merged.** This is
independent of which licence is chosen and it is the thing that actually preserves the option of a
premium closed build later: that option exists only because the author holds all the copyright,
and it ends quietly at the first merged PR from someone else. Retroactive relicensing is a
negotiation with everyone who ever contributed; up-front is a checkbox. The cost is real —
contributors bounce off paperwork for a typo fix — and it is the price of keeping the option.

Tasks: `LICENSE` (MPL 2.0) and `LICENSE-APACHE` at the root with a `README` note on which applies
where, Exhibit A headers in `src/` files (trivial here, since every file already opens with a long
explanatory header), and a `CONTRIBUTING.md` carrying the agreement.

**None of this is legal advice, and a dual-licensing plan is worth twenty minutes of a real
lawyer's time before the first binary ships.**

---

## 6. Documentation

Today's `README.md` is a design summary written for the author: it opens with the authoritative
design document, three architectural commitments and a protocol reference. Excellent, and not what
someone who just installed something needs.

* **`README.md`, restructured:** what it is and what it does to your machine → **you need a
  server, here is why and how** → install → first run → the hotkey and the pill → what to do when
  it doesn't work. Design content moves under a "How it works" heading that links `plan.md`.
* **`docs/setup.md`, new:** standing up an endpoint. Which existing servers can sit behind the
  protocol, the worked example, and the honest statement that this is the part the project does
  not do for you yet.
* **`docs/protocol.md` promoted:** it stops being an internal note about a pipe to a child process
  and becomes the public API. Rewrite §2, add the network binding, answer §1.4's two gaps, and add
  the one thing a spec for outsiders needs that an internal one does not — a minimal worked
  example of a conforming server.
* **`docs/supported-targets.md` exists** and is genuinely useful to users. Link it from the README
  as "where this works and where it doesn't", which is the honest framing anyway.
* **The privacy statement has to be exact.** The README currently promises "No cloud, no
  telemetry, no network access during dictation." Under this architecture the third clause is
  false by design — dictation *is* a network conversation now. Rewrite it to what is actually
  true: no telemetry, no cloud service, audio goes to the server you configure and nowhere else,
  unencrypted, on your network.

---

## 7. Phases and gates

Each phase ends on a demonstration, not an opinion — the shape `plan.md` §7 uses.

| Phase | Work | Gate |
|---|---|---|
| **R0 — decide** | Certificate route chosen and started; lead time is weeks, so this is first, not last. `LICENSE` files landed. Spec rewrite for the network binding (§1.3, §1.4). | A signed hello-world executable downloads and runs on a clean VM with no SmartScreen block. The spec rewrite is reviewed by someone implementing a server against it. |
| **R1 — the transport swap** | WebSocket binding in `ipc_client.cpp`; socket transport in the reference `server/`; spawn machinery behind a dev flag. | A full dictation session, client to server over `ws://`, no child process and no Python on the client machine. 479 tests still green. |
| **R2 — configuration** | `ServerConfig`, the Server settings page with a working Test button, the no-server state, first-run flow. | Someone who has never seen the repo installs, types a URL, presses Test, and dictates. Observed, not asserted. |
| **R3 — package** | Inno Setup script, `/MT`, manifest, icon, version resource, signing of all three artefacts, Defender pre-submission. | Clean-VM install → dictate → uninstall, Defender and SmartScreen at defaults. No warnings, nothing left behind. |
| **R4 — document and beta** | README rewrite, `docs/setup.md`, privacy statement, `CONTRIBUTING.md`. Three to five outside testers **who already run a server** — that is the v1 audience. | Every tester reaches working dictation using only the README. Each failure becomes a README fix or a bug, and the phase repeats until nobody needs to ask the author. |
| **R5 — release** | Tag, publish, download page, version-check endpoint. | — |

R0 runs in parallel with R1. The certificate is the long pole and nothing else depends on it
until R3.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| **The first-run cliff: "install this, then go set up a server" loses most people** | Accepted for v1, and it defines the audience — R4 recruits testers who already run a server. The reference server (§1.5) is the fix and it is a separate deliverable. This is the single biggest adoption risk in the document and it is a deliberate choice, not an oversight. |
| Defender or SmartScreen quarantines a signed build anyway | R0's gate is exactly this test; pre-submit to Microsoft; EV if OV reputation does not accumulate |
| A batch model re-punctuates and trips the committed-prefix invariant | Observed 2026-08-18 and understood; v1 defaults to finalize-only and end-of-session cadence; live preview is opt-in and labelled |
| Users point the app at an internet endpoint and send their microphone to a third party | Warning line for non-private hosts (§2.4), plain statement in the README, `https://` accepted so the careful user has a path |
| A session dies whenever the server hiccups, and it now can | `on_sidecar_lost` already preserves committed text and loses only the tail; R1 adds reconnect-with-backoff and a tray state that says which |
| No offline path at all in v1 | Stated in §0 as a cost of the architecture, not hidden. `tools/kvit-speech` still exists for whenever a local server appears |
| The protocol becomes public and then needs a breaking change | §5 of the spec already has version negotiation; use it, and treat the first published version as load-bearing from the day it ships |
| A merged contribution forecloses the premium build option | The contributor agreement, in place *before* the first PR (§5) |
| MSIX turns out to be needed for a Store listing later | Not v1; the `/MT` and manifest work in R3 is a prerequisite either way |

---

## 9. Open questions

1. **Concurrency and model discovery in the spec** (§1.4) — decided in R0, because a published
   protocol is expensive to change afterwards.
2. **Does the Test button need model discovery to be useful**, or is a free-text model field
   enough for v1? This is the main thing pulling `hello_ack.models[]` into R0 rather than v1.1.
3. **Who is the v1 user?** The plan assumes "someone who already runs a local ASR server". That is
   a small audience, and it is the honest consequence of shipping no server. If the answer is
   meant to be broader, the reference server moves from §1.5 into the release and R1 grows.
4. **Does `tools/kvit-speech` get archived or maintained?** It is the seed of an offline story and
   it is currently in nobody's phase.

---

## 10. What was built, 2026-08-28

Written after the fact, against the plan above, and specific about the difference between "done"
and "done and observed". Every claim below is either verified by a test that runs or is marked as
unverified; there is no third category.

### Built and verified

| § | Work | How it was verified |
|---|---|---|
| §1.3 | The WebSocket binding, client side (`IpcClient::connect`, WinHTTP) | `kd_probe --server` completes the upgrade, handshake and `ready` against two independent servers |
| §1.3 | The WebSocket binding, server side (`kvit_asr/ws.py`, `--listen`) | 29 new tests drive it over a real socket with a client written from the spec |
| §1.4 | Concurrency (§2.7) and model discovery (§5.1) answered in the spec and implemented on both sides | `server_busy` and `model_unavailable` are asserted end to end, in both suites |
| §2.2 | `ServerSettings`, the Server settings page, the Test button | 7 new settings tests; the Test button's call (`probe_server`) is the same one `kd_probe --server` makes and that is exercised. **Extended 2026-08-28**: `server.mode` distinguishes a speech service from a protocol server — necessary because `parse_server_url` treats `http://` as a WebSocket synonym and the two cannot be told apart by looking — and in speech-service mode Test launches a private `kvit-asrd.exe` and handshakes *it*, so a green Test covers the whole path. 11 further settings tests, including every migration case for a file written before the field existed. |
| §2.4 | The non-private-host warning | Unit-tested against loopback, all three RFC 1918 ranges, CGNAT, link-local, IPv6 unique-local, and the public addresses that must *not* be excused |
| §3.2 | `/MT`, the application manifest, the icon, the version resource | The release script greps the binary for CRT imports and refuses to package if it finds any |
| §1.2 | Spawn machinery out of shipped builds (`KD_ENABLE_SIDECAR_SPAWN=OFF`) | ~~The release script greps for `CreateProcessW` and `AssignProcessToJobObject`~~ — **superseded 2026-08-28.** A shipped client now contains both, because it launches `kvit-asrd.exe` (`openai-translator.md` §4.6). The script instead checks that the flag is still OFF, that `kvit-asrd.exe` is staged, and that the client does **not** contain the string `spawned sidecar pid` — the unique marker of the `models.json` interpreter path, which is what §1.2 was about. A string literal is compiled out with the code that uses it, so its absence is a statement about what the build can do rather than about which APIs it imports. |
| §3 | The per-user installer | Installed silently, inspected, launched, uninstalled: program directory, Start Menu group, Run key and uninstall entry all created and all removed, and `settings.json` correctly kept |
| §5 | MPL-2.0 / Apache-2.0 split, headers, CLA | `tools/apply-license-headers.sh --check` runs clean |
| §6 | README rewrite, `docs/setup.md`, the corrected privacy statement | — |
| §1.5 | Not the reference server, but `docs/examples/minimal_server.py`: a complete conforming server in 400 lines of standard library | Executed by 27 tests on every run, including the RFC 6455 vector that caught a wrong magic GUID |

Totals: 494 C++ tests, 475 server tests, 129 bench tests. All green.

### Built but not observed

* **A full dictation session over the WebSocket binding.** Every layer under it is exercised —
  the upgrade, the framing, the handshake, `start`, audio, `stop`, `final`, `stopped` — and the
  server side is driven through exactly that sequence by the test suite. What has not happened is
  a person pressing the hotkey and watching words appear from a network server. That is R2's gate
  and it needs a person.
* **The Test button in the dialog.** The function behind it is exercised through `kd_probe`; the
  button itself, its worker thread and its generation counter are not.
* **The installer on a *clean* machine.** It has been installed, launched and uninstalled here
  (see the table above), which proves the script and the payload. It has not been through R3's
  actual gate: a browser download with Mark-of-the-Web intact, on a machine with no Visual Studio,
  no exclusions and Defender at its defaults.

### Deliberately not done

* **Signing (§4).** No certificate exists. This is the largest risk in the release and it is
  carried, not solved. Nothing in the build pretends otherwise: there is no `SignTool` directive
  and the release script says so in its closing output.
* **The clean-VM test (§3, R3's gate).** Requires a VM, a browser download with Mark-of-the-Web
  intact, and Defender at its defaults. The development machine has exclusions that hide exactly
  the failure the test exists to find.
* **Defender pre-submission (§4 item 3).** Follows the certificate.
* **R4's outside testers.** Three to five people who already run a server. That is a recruitment
  problem, not a code one.
* **A version-check endpoint and a download page (R5).** Both need infrastructure that does not
  exist.
* **The reference server (§1.5).** Still a separate deliverable on its own timeline. What exists
  instead is `docs/examples/minimal_server.py`, which proves the protocol without transcribing
  anything, and `server/` with `--listen`, which is a real server for anyone who already has a
  model.

### Two things this work changed about the plan itself

1. **§1.2 said `docs/protocol.md` §2 would be "replaced" by a network binding.** It was extended
   instead: §2.1 now routes the reader, §2.6–§2.8 are the network binding, and §2.2–§2.5 remain
   as the pipe binding the bench uses. Replacing them would have invalidated about sixty
   `§2.x` references in source comments across both languages, for no gain — and keeping one
   binding alive is what keeps "the framing layer is transport-agnostic" a demonstrated claim
   rather than an asserted one.
2. **§1.4 offered two options for model discovery and called the second "v1-acceptable".** The
   first was taken (`hello_ack.models[]`), because it turned out to cost about forty lines per
   side, and the free-text fallback survives as what a client does when a server advertises
   nothing. A server that implements neither field is still fully conforming.
