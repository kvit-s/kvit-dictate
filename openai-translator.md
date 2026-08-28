# `kvit-asrd` — the translator

Status: **plan, nothing built.** 2026-08-28, against commit `25d036d`.

This document restores a decision that was made, reversed, and then missed. It is effectively
**revision 3 of `release-plan.md` §1**, and it exists because revision 2 produced a working
release that is wrong for the person it was built for.

---

## 0. What went wrong, first, because it is the reason for everything below

Revision 1 of `release-plan.md` (commit `59b0a57`) said:

> Ship **`kvit-asrd.exe`** — a native sidecar that speaks the same `docs/protocol.md` the Python
> one does, built from this repo, installed beside `kvit-dictate.exe`.

with a port table whose v1 row for `backends/openai_http.py` read *"Port to WinHTTP.
`src/normalizer.cpp` already does exactly this shape of thing."*

Revision 2 (`c6526cc`) deleted it, on the argument that shipping any server at all was scope the
release could not carry: *"the release ships the client; the server is configuration, not a
payload."* That argument is coherent and it was implemented faithfully — the client now speaks a
published WebSocket protocol to an endpoint the user names, and that part is good and stays.

**What it got wrong is the assumption underneath it: that the user's endpoint speaks this
protocol.** It does not. The actual endpoint on this network is
`http://strixhalo.lan:10301`, an OpenAI-compatible `/v1/audio/transcriptions` service. Under
revision 1 the client spawned a translator; under revision 2 the translator still exists, still
runs on the user's machine, and is now started by hand from a `.bat` file. The moving parts did
not go away — the supervision of them did. From the user's seat that is strictly worse than
where they started, and "install one executable, say where the service is" became "install one
executable, keep a Python window open".

So: the protocol boundary was the right call and stays. Making the *other side of it* the user's
problem was not.

---

## 1. The decision

### 1.1 What `kvit-asrd` is

**A second native executable, shipped beside `kvit-dictate.exe`, built from this repository.**
It speaks `docs/protocol.md` on one side and one or more ASR backends on the other. For v1 there
is exactly one backend: **OpenAI-compatible HTTP**.

```
┌──────────────────────┐         ┌──────────────────────┐        ┌────────────────────┐
│  kvit-dictate.exe    │   ws    │   kvit-asrd.exe      │  HTTP  │  your ASR service  │
│                      │ ──────► │                      │ ─────► │                    │
│  hotkey, capture,    │ 127.0.0.1│ protocol server     │        │  /v1/audio/        │
│  session, commit,    │ ◄────── │  endpointing (VAD)   │ ◄───── │    transcriptions  │
│  injection, overlay  │  frames │  stability           │  JSON  │                    │
└──────────────────────┘         │  backends/           │        └────────────────────┘
                                 └──────────────────────┘
       ~1.2 MB                          ~1 MB, no Python                 yours
```

Both halves ship in the installer. Neither needs Python. The user configures **one address**:
where their speech service is.

### 1.2 Why C++ in this repository

Go was seriously considered and is recorded in §1.3. C++ wins on one fact that only became clear
while writing this plan: **most of what the translator needs is already written, in C++, and
already tested.**

| The translator needs | Where it already exists | Lines |
|---|---|---|
| Frame codec, both directions, all twelve message types | `src/protocol.cpp` — bidirectional by construction; the client encodes what a server decodes and vice versa | 1,309 |
| The shared conformance corpus | `tests/fixtures/protocol/` — 21 cases, already run by `tests/test_fixtures.cpp` | 21 files |
| UTF-8 and grapheme-cluster offsets | `src/util/utf8.cpp` — including `common_grapheme_prefix(items, count)`, which *is* step 2 of the stability algorithm over N hypotheses, plus `retract_graphemes`, `retract_to_word_boundary`, `trim_trailing_whitespace`, `floor_grapheme` | 796 |
| The six-step stability algorithm | `src/commit_engine.cpp` — implemented and tested, from the client's side of the same computation | 818 |
| An HTTP client that POSTs to a configured endpoint on a worker, cancellable, with a "test this now" path | `src/normalizer.cpp` | 618 |
| RFC 6455 framing, masking, continuation reassembly | `src/ipc_client.cpp` (client role) and `docs/examples/minimal_server.py` (server role, in a language that will not link) | — |

`stability.py` is 871 lines, but roughly 350 of those are the UTF-8/grapheme layer that
`util/utf8.cpp` already provides. What actually has to be *written* is the server-side tracker
around those primitives — a few hundred lines, not nine hundred.

The other three arguments, from revision 1 and still true:

* **Crash isolation is load-bearing.** `App::on_sidecar_lost` already handles a dead engine by
  keeping committed text and losing only the tail. Folding ASR into the client means a backend
  fault takes the user's dictation and the tray icon with it.
* **The Python sidecar keeps working, unchanged**, for the bench and for every backend that needs
  weights. `plan.md` §4 is a measurement programme and must not be collateral damage.
* **A second client (macOS) gets a defined thing to talk to** — and now a *shipped* thing.

### 1.3 Why not Go, recorded because it was close

Go's case was real: `net/http` + `mime/multipart` + `encoding/json` make the backend about 120
lines against WinHTTP's ~300 fiddly ones, goroutines map cleanly onto per-connection servers,
and a Go binary cross-compiles to Linux — which would let the translator run *on* the ASR box
rather than on the desktop.

It loses on three counts:

1. **It throws away the reuse in §1.2.** The codec becomes ~600 new lines and the shared fixture
   corpus needs a third runner. Two implementations of a spec disagreeing is the failure the
   corpus exists to catch; three implementations is more surface, not less.
2. **Local backends later become cgo.** Every serious local ASR runtime — whisper.cpp,
   sherpa-onnx, ONNX Runtime — is a C or C++ library. §1.5 of `release-plan.md` still wants them
   eventually, and cgo is the worst of both languages for that.
3. **A third toolchain in a repository whose build rule is "no third-party dependencies of any
   kind"** (`CMakeLists.txt:1-9`). Adding a language is a bigger change than adding a target.

The cross-platform argument is the one that genuinely hurts to give up, and §10 keeps it open:
nothing here prevents a Go or Rust translator later against the same published protocol. That is
what publishing the protocol was for.

### 1.4 What v1 deliberately does not contain

* **Any backend but OpenAI-compatible HTTP.** Not local models, not `windows-speech`, not
  sherpa. Each is a separate, later, argued addition.
* **TLS on the listening side.** It binds loopback (§4.6); `wss://` is a reverse proxy's job
  (`docs/protocol.md` §2.8).
* **Authentication.** Loopback only. Unchanged from §2.8.
* **A settings UI of its own.** Configuration arrives on the command line from the client that
  launched it, or from a config file for someone running it standalone (§4.6).

---

## 2. Architecture

### 2.1 Process and threads

One process, one listening socket, one connection at a time for v1 (`--max-connections`,
defaulting to 1, refusing extras with `server_busy` per `docs/protocol.md` §2.7).

| Thread | Owns | Blocking? |
|---|---|---|
| main | the listener, config, lifetime, the parent-liveness watch | `accept()` |
| connection | one client: framing, session state, endpointing, stability, all outbound frames | reads |
| decode | one HTTP request at a time to the backend | yes, and that is why it is not the connection thread |

The decode thread is not an optimisation. A transcription round trip against the measured
endpoint is **237–541 ms** (and the optional VAD gate a further 50–176 ms); doing that on the
connection thread would stall audio intake for half a second at a time and overflow the
`AUDIO_QUEUE_MAX_MS` budget that `docs/protocol.md` §9 makes fatal.

### 2.2 Layers, and which existing file each maps to

```
L0  transport     Winsock listener + RFC 6455 server        NEW      §4.1
L1  framing       proto::FrameReader / encode_control_frame  REUSE    src/protocol.cpp
L2  encoding      proto::decode_control / Message            REUSE    src/protocol.cpp
L3  session       start/stop/cancel routing, one at a time   NEW      §4.2
    endpointing   silence detection → utterance boundaries   PORT     §4.4
    stability     N hypotheses → stable_bytes                PORT     §4.3
    backend       PCM → text                                 PORT     §4.5
```

---

## 3. The port table

Honest line counts, and what each row actually costs.

| Source | Lines | Disposition |
|---|---|---|
| `protocol.py` | 1,891 | **Not ported.** `src/protocol.cpp` is the same spec in C++, bidirectional, and the fixtures test it. |
| `transport.py` | 800 | **Not ported.** Named pipes; the translator listens on a socket. |
| `ws.py` | ~400 | **Reimplemented in C++** (§4.1). The Python file remains the reference and the second implementation. |
| `stability.py` | 871 | **Port the tracker only, ~250 lines.** The grapheme/UTF-8 half is `util/utf8.cpp`, and the algorithm is `commit_engine.cpp`'s, run from the other side. |
| `vad.py` | 1,204 | **Port `EnergyZcrScorer` + `VadEndpointer`, ~350 lines.** Silero is out of v1 — see §4.4, which is blunt about what that costs. |
| `session.py` + `connection.py` | 1,352 | **Port the routing, ~400 lines.** Much of this is CLI surface and backend-selection machinery a fixed-backend exe does not need. |
| `backends/base.py` `PseudoStreamingBackend` | ~250 of 786 | **Port, ~200 lines.** The overlapping-window driver, including the prefix-freeze on window slide, which is the subtle part. |
| `backends/openai_http.py` | 407 | **Port to WinHTTP, ~350 lines.** Multipart is hand-rolled on both sides; `normalizer.cpp` is the shape to copy. |
| `server.py` | 1,197 | **Port the run loop, ~200 lines.** |

Rough total of new C++: **1,750–2,000 lines**, against roughly 3,500 lines of reused, tested code.

---

## 4. The pieces

### 4.1 The WebSocket server (`src/asrd/ws_server.{h,cpp}`)

The one piece of genuinely new transport code, and the reason is worth stating: **WinHTTP is a
client API.** It gave the *client* its WebSocket for free (`ipc_client.cpp`); there is no
server-side counterpart short of `http.sys`, which means an HTTP namespace reservation and, on a
non-loopback binding, elevation. Neither is acceptable for a program whose installer is
deliberately per-user and prompt-free.

So: raw Winsock, and RFC 6455 by hand. ~300 lines, and it is well-trodden ground here —
`docs/examples/minimal_server.py` is exactly this, executed by 27 tests, and
`server/kvit_asr/ws.py` is the same again. The C++ version is a transcription of a design that
has been validated against a real WinHTTP client twice.

What it must get right, all of which the two Python versions already encode:

* `Sec-WebSocket-Accept` = base64(SHA-1(key + `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)). **That
  GUID is written here because getting it wrong from memory cost an afternoon**: both endpoints
  compute a matching-looking handshake and WinHTTP rejects the upgrade with "invalid server
  response". The RFC's own test vector goes in the unit test, not just the constant.
* Echo the `kvit-dictate.v1` subprotocol when offered, never invent it (§2.6.2).
* Reject unmasked client frames (RFC 6455 §5.1).
* Reassemble continuation frames before decoding (§2.6.3).
* Binary only; a text message is fatal `unknown_frame_type` even when its bytes would decode.
* Validate the redundant 4-byte length prefix against the message size (§2.6.3).
* Answer a plain HTTP request with a 400 that says what the port is — somebody will paste the URL
  into a browser.

SHA-1 comes from `bcrypt.dll` (`BCryptCreateHash`), which `CMakeLists.txt` already links for
`BCryptGenRandom`. Base64 from `crypt32.dll` (`CryptBinaryToStringA`) or twenty lines by hand.
No new dependency either way.

### 4.2 The session loop (`src/asrd/session_server.{h,cpp}`)

The server-side mirror of `src/session.cpp`. One session per connection (§7.2), and the state
machine is small because the protocol is:

```
hello          → validate proto; hello_ack (with models[]); ready
start          → open session, reset endpointer and stability, utterance_id = 1
audio (0x02)   → append to the utterance buffer; feed the endpointer
  endpoint     → close utterance, emit final, ++utterance_id
  hop elapsed  → decode window, emit partial
stop           → flush, decode the remainder, final, stopped
cancel         → drop everything, stopped(reason=cancelled), no further partial/final
discontinuity  → flush the decoder's state
```

Every rule that is easy to get wrong is already written down and already has a fixture:
`session_id` echoed verbatim and never normalised (§7.1); `seq` strictly increasing per session
(§7.4); `stopped` always emitted, cancelled or not (§10.4); `stop` for an unknown session
answered `stopped(reason=unknown_session)` rather than an error (§10.5).

### 4.3 Stability, from the server's side (`src/asrd/stability.{h,cpp}`)

`plan.md` §1.4's six steps, which `commit_engine.cpp` already implements from the client's side:

1. Keep the last **N** hypotheses for the current utterance (N = 2).
2. Candidate = longest common prefix over those N, **on grapheme clusters** —
   `utf8::common_grapheme_prefix(items, count)`, which already exists and takes an array.
3. Retract at least **M** clusters behind the decode head (M = 3) — `utf8::retract_graphemes`.
4. Retract to the last word boundary and hold back trailing whitespace —
   `utf8::retract_to_word_boundary`, `utf8::trim_trailing_whitespace`.
5. Require the candidate unchanged for **T** ms (T = 200).
6. Never decrease, and never let the bytes below the published mark change.

Step 6 is not advisory. `stability.py`'s own docstring puts it best and it transfers verbatim:

> The server must NEVER emit a `stable_bytes` that decreases, or one whose prefix bytes differ
> from what it already sent. […] the Client's required response is to *preserve the document* and
> never repair, which means the wrong bytes stay in the user's file forever. Catching it here
> costs one comparison; catching it on the Client costs a corrupted document.

So the C++ tracker keeps the same shape: a `check_emission(text, stable_bytes)` that **refuses**
rather than clamps, on the path out to the wire, and a `stability_violation` error rather than a
silently-adjusted number. Clamping is forbidden by §8.3 because it hides the defect ICER exists
to measure.

**The batch-model caveat, which is why this matters immediately.** A batch model re-punctuates
earlier words as later context arrives, so it violates monotonicity *structurally*, not
occasionally — `models.json` records a violation within seconds of the first human run. v1
therefore defaults this backend to **finalize-only**: one `final` at the endpoint, no `partial`s,
`ready.mode = "pseudo-streaming"`. Live preview stays available, off by default, and labelled.
The stability tracker is still built and still runs, because the moment a streaming backend
arrives it is the thing standing between the model and the user's document.

### 4.4 Endpointing (`src/asrd/vad.{h,cpp}`)

Port `EnergyZcrScorer` and `VadEndpointer` — frame energy in dBFS plus zero-crossing rate, a
logistic on the pair, hysteresis into speech/silence states, and an utterance boundary after
`endpoint_silence_ms` (default 1500 for this backend).

**Silero is out of v1, and `release-plan.md` §1.6 already established what that costs — which is
nothing, because it was never running.** `config.py:248` defaults `--vad auto`, `--silero-model`
defaults to `None`, no `models.json` entry sets it, and `SileroOnnxScorer` imports `onnxruntime`
inside `load()`, so on every run to date it has failed to load and fallen back to the energy
scorer. **That is what has been endpointing every dictation this project has ever done.** Porting
the energy scorer is therefore not a downgrade; it is porting the thing that is actually in use.
Its own file calls it "not a substitute for Silero" and that remains true and remains a §10 item.

Separately and confusingly, `models.json` sets `vad_gate=true`, which is a *different* mechanism:
a call to the **server's** Silero over `/v1/audio/vad`. That one is ported (§4.5) because it is
doing real work — it is what stops 500 ms of silence being transcribed as "Yeah."

### 4.5 The OpenAI-compatible backend (`src/asrd/backend_openai.{h,cpp}`)

`POST {base_url}/v1/audio/transcriptions`, `multipart/form-data`, fields `model`, `language`,
`response_format=json`, plus a `file` part holding the utterance as a WAV. Reply is JSON with a
`text` field.

* **WAV, not a compressed format.** The standard library can write it, it is lossless, and
  re-encoding would put a codec between the measured audio and the model for no benefit.
* **Multipart by hand**, as in Python, with a fixed boundary. A WAV cannot contain the token
  because its body is length-prefixed rather than delimited.
* **WinHTTP**, following `normalizer.cpp`: synchronous on a worker, per-operation timeouts, and
  the same "test this now" path that the settings dialog's Test button uses.
* **The VAD gate**, optional and on by default for this backend: `POST /v1/audio/vad` first, skip
  the transcription when it says no. **It fails open** — a missing route (503), a timeout, a
  malformed answer all mean "go ahead". Losing something the user actually said is worse than an
  occasional invented word, and a VAD outage must never be able to silence dictation.
* **The pseudo-streaming window**, ported from `PseudoStreamingBackend`: re-transcribe the whole
  window every `hop_ms` and emit the result as the cumulative hypothesis. Past `window_ms` the
  oldest audio scrolls out and its transcript must be **frozen** into a prefix, estimated by
  word-overlap stitching — which can be wrong, in ways `base.py` documents honestly and which
  must be copied across with the code rather than left behind with it.

### 4.6 Configuration and how it is started

This is the part the whole document exists for. **The user configures one address.**

`kvit-dictate.exe` gains a "local translator" mode in Settings → Server:

```
Speech service   [ http://strixhalo.lan:10301        ]   ← the only thing most people type
                 ( ) I run my own kvit-dictate server → ws://...
                 [Test]
```

In local mode the client **launches `kvit-asrd.exe` from its own directory**, on loopback, on an
ephemeral port, with the service URL on its command line, and supervises it for its lifetime.

**Yes, this reinstates process spawning that `release-plan.md` §1.2 deleted, and the distinction
is the whole point.** What §1.2 removed was spawning *an arbitrary interpreter named in a
user-editable `models.json`, with a user-supplied argv and environment*. What this adds back is
launching **one fixed executable, by absolute path, from the installation directory, with a
command line this program constructs**. The first is a configuration file that can execute
anything; the second is two halves of one application. `KD_ENABLE_SIDECAR_SPAWN` stays off — this
is a new, narrow path, not the old one re-enabled.

The supervision rules are `docs/protocol.md` §10.3 and the existing job-object discipline: the
child dies with the parent (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), so a crashed client can never
orphan one; a dead child is a lost connection with backoff, which is already implemented.

Standalone use stays supported and is how the translator runs on another machine:

```
kvit-asrd.exe --listen 127.0.0.1:10402 --service http://strixhalo.lan:10301 [--model ...]
```

---

## 5. Testing

* **The shared fixture corpus, from the other role.** `tests/test_fixtures.cpp` already drives
  the 21 cases as a client. The translator adds a *server-role* runner over the same files. Three
  implementations of one spec now exist (C++ client, C++ server, Python server) and the corpus is
  what keeps them honest.
* **In-process loopback.** `IpcClient::connect` against a translator listening on an ephemeral
  port, in the same test binary: full handshake, a session, audio, `stop`, `final`, `stopped`.
  The equivalent already exists for the pipe binding (`attach_connected`), so the shape is known.
* **A stub HTTP service.** The backend is tested against a local listener that returns canned
  JSON, including the failure paths: 503 on the VAD route (must fail open), a 500 on
  transcription (must be `decode_failed`, non-fatal, session aborted), a non-JSON body, and a
  timeout.
* **Hostile stability.** `server/tests/test_hostile_stability.py` already exists to force illegal
  `stable_bytes` onto the wire so §8.3 can be exercised end to end. The C++ translator needs the
  same switch, with the same name and the same loud logging, and it must not be reachable from
  backend options — a backend must never be able to authorise its own protocol violations.

## 6. Packaging

Two executables instead of one. Nothing else about `release-plan.md` §3 changes: `/MT` on both,
the manifest on the client (the translator has no windows and needs only `asInvoker`), the same
per-user Inno Setup script with one more `[Files]` entry, the same portable ZIP.

The ZIP grows from ~0.63 MB to roughly 1.1 MB. Still one download, still no Python, still no
runtime to install.

## 7. Phases

| Phase | Work | Gate |
|---|---|---|
| **T0** | `ws_server` + the fixture corpus in the server role; a translator that completes a handshake and answers `ready` with a stub backend | `kd_probe --server ws://127.0.0.1:10402/` against `kvit-asrd.exe`, and the 21 fixtures green in both roles |
| **T1** | Session loop, endpointing, the OpenAI backend in finalize-only mode | A full dictation into Notepad, through `kvit-asrd.exe`, against strixhalo, with no Python running anywhere. Observed, not asserted. |
| **T2** | Stability tracker + pseudo-streaming window; live preview available and off by default | Live preview against a batch model produces either a conforming stream or a refused emission — never a `stability_violation` on the client |
| **T3** | The client launches and supervises it; Settings → Server becomes one address | Install from the installer on a machine with no Python, type `http://…:10301`, press Test, dictate |
| **T4** | Retire `run-server.bat` | It is deleted, and nothing needs it |

## 8. Risks

| Risk | Mitigation |
|---|---|
| The pseudo-streaming freeze-on-slide heuristic mis-stitches and commits a wrong prefix | Inherited from `base.py` along with its documented failure cases. v1 defaults to finalize-only, where the window never slides because the utterance ends first. |
| Porting `stability.py` introduces a divergence the fixtures do not catch | The fixture corpus covers framing and session ordering, not the stability *computation*. T2 needs property tests comparing the C++ tracker against the Python one over the same hypothesis sequences — the two must agree byte for byte. |
| Hand-rolled RFC 6455 on the server side has a bug the Python version does not | It is written third, against two working implementations and a corpus, and tested against a real WinHTTP client rather than against itself. |
| Spawning a child undoes §1.2's attack-surface reduction | Narrowed deliberately: one fixed path, no user-supplied argv, no environment mapping, no `models.json`. `KD_ENABLE_SIDECAR_SPAWN` stays off. |
| The energy endpointer is worse than Silero and now it is the only one | It is already the only one, and has been for every dictation this project has run. Stated, not discovered. Silero is a §10 item. |
| Two binaries doubles the signing work when a certificate exists | True and cheap; both are signed by the same script. |

## 9. What this does not change

`docs/protocol.md` is untouched. No new message, no new field, no proto bump. That is the test of
whether the boundary was drawn in the right place, and it passes: a whole new server, in a
different language, with a different backend, and the spec does not move.

`server/` keeps working, unchanged, for the bench and for every backend that needs weights.
`--listen` stays. The Python server remains the reference implementation for anything
`kvit-asrd` does not do, which for v1 is most things.

## 10. Open questions

1. **Silero in the translator.** ONNX Runtime is a real dependency and `CMakeLists.txt` forbids
   third-party libraries outright. Reaching the server's `/v1/audio/vad` covers the case that
   matters today. Revisit when a local backend arrives, since that breaks the rule anyway.
2. **Does the client still need its own settings for stability params?** They are the *server's*
   computation now. `Settings::stability` is currently client-side and feeds `CommitEngine`; the
   translator will want its own copy, and two places to set N/M/T is one too many.
3. **Ephemeral port or fixed?** Ephemeral avoids collisions and makes two clients on one machine
   work; fixed makes `kd_probe` and a support conversation simpler. Probably ephemeral, with the
   chosen port logged and shown in the settings dialog.
4. **Cross-platform later.** Nothing here forecloses a Go or Rust translator against the same
   protocol, and §1.3's cross-compilation argument is the one real thing C++ gives up. If the
   translator ever wants to live on the ASR box rather than the desktop, that is the moment to
   revisit — not now, and not by rewriting this one.
5. **Does `models.json` survive at all?** It is already gone from shipped builds. Once the bench
   is the only consumer, it may belong under `bench/` rather than at the repository root.
