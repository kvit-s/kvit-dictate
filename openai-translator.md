# `kvit-asrd` — the translator

Status: **built.** 2026-08-28. T0–T4 are implemented and committed; the sections below are kept
as written, because a plan that is quietly edited to match what was built stops being a record of
what was decided.

What changed against the plan while building it, all of it small:

* **`serve_connection` lives in `src/asrd/connection.{h,cpp}`, not in `main.cpp`.** §5 asks for an
  in-process loopback test, and a serve loop inside `main.cpp` is reachable only through
  `CreateProcess`.
* **`kd::log` grew a stderr sink**, and its first `init()` now reaches the sinks before the first
  line is emitted. §4.8 says asrd logs to stderr; there was no stderr sink, and the opening lines
  of *every* process were going to the pre-init defaults.
* **There is no `overload_audio` in asrd**, and `session_server.h` says why: the connection thread
  never waits on the network, so §9's inbound queue does not exist to overflow. The buffer that
  does grow is bounded by `VadConfig::max_utterance_ms`, which is a different mechanism.
* **`Settings::normalise()` reconciles `server.mode` with `server.url`** when they cannot both be
  right, so `serialize`→`parse` stays total. §4.6 specified the migration for a file with no mode
  and not the impossible combination.
* **A second fixture corpus**, `tests/fixtures/stability/`, generated from the Python tracker.
  §5 asks for property tests against it; the protocol corpus cannot express them.

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
is exactly one backend: **OpenAI-compatible HTTP**. Licence is MPL-2.0, like the rest of `src/`.
The protocol spec and the Python reference server stay Apache-2.0; this binary is half of the
shipped application, not a second public protocol.

```
┌──────────────────────┐         ┌──────────────────────┐        ┌────────────────────┐
│  kvit-dictate.exe    │   ws    │   kvit-asrd.exe      │  HTTP  │  your ASR service  │
│                      │ ──────► │                      │ ─────► │                    │
│  hotkey, capture,    │ 127.0.0.1│ protocol server     │        │  /v1/audio/        │
│  session, commit,    │ ◄────── │  endpointing (VAD)   │ ◄───── │    transcriptions  │
│  injection, overlay  │  frames │  stability           │  JSON  │                    │
└──────────────────────┘         │  backends/           │        └────────────────────┘
                                 └──────────────────────┘
       ~1.2 MB                          headless, no Python              yours
```

Both halves ship in the installer. Neither needs Python. The user configures **one address**:
where their speech service is.

### 1.2 Why C++ in this repository

Go was seriously considered and is recorded in §1.3. C++ wins on one fact that only became clear
while writing this plan: **the codec, the grapheme layer, and the fixture corpus are already
written, in C++, and already tested.** That is the reuse. The rest is a port, not a link.

| The translator needs | Where it already exists | Lines (this tree) | What "reuse" actually means |
|---|---|---|---|
| Frame codec, both directions, all twelve message types | `src/protocol.cpp` | 1,414 | **Link.** Bidirectional by construction; the client encodes what a server decodes and vice versa. |
| The shared conformance corpus | `tests/fixtures/protocol/` | 21 files | **Run from the other role.** `tests/test_fixtures.cpp` already drives them as a client. |
| UTF-8 and grapheme-cluster offsets | `src/util/utf8.cpp` | 799 | **Link.** Includes `common_grapheme_prefix(items, count)`, which *is* step 2 of the stability algorithm over N hypotheses, plus `retract_graphemes`, `retract_to_word_boundary`, `trim_trailing_whitespace`, `floor_grapheme`. |
| Server-side `stable_bytes` tracker | `server/kvit_asr/stability.py` | 874 | **Port the tracker, ~250 lines**, around the utf8 primitives. `src/commit_engine.cpp` (821) is the *client* of that mark — it consumes `stable_bytes` and emits injection ops. It is not a library the translator calls. See §4.3. |
| An HTTP client that POSTs to a configured endpoint on a worker, cancellable | `src/normalizer.cpp` | 621 | **Shape to copy, not a library to call.** The normalizer talks to a different URL with a different body. WinHTTP-on-a-worker, timeouts, and cancellation (`WinHttpCloseHandle`) are the parts that transfer. |
| RFC 6455 client | `src/ipc_client.cpp` | — | **Reference, client role only.** WinHTTP. The translator is the server; see §4.1. |
| RFC 6455 server | `docs/examples/minimal_server.py`, `server/kvit_asr/ws.py` (436) | — | **Transcription into C++.** Validated against a real WinHTTP client twice; will not link. |

`stability.py` is 874 lines, but roughly 350 of those are the UTF-8/grapheme layer that
`util/utf8.cpp` already provides. What actually has to be *written* is the server-side tracker
around those primitives — a few hundred lines, not nine hundred.

The other two arguments, from revision 1 and still true:

* **Crash isolation is load-bearing.** `App::on_sidecar_lost` already handles a dead engine by
  keeping committed text and losing only the tail. Folding ASR into the client means a backend
  fault takes the user's dictation and the tray icon with it.
* **The Python sidecar keeps working, unchanged**, for the bench and for every backend that needs
  weights. `plan.md` §4 is a measurement programme and must not be collateral damage.

A second client (macOS) is **not** a reason to write this binary. asrd is Windows C++ and binds
loopback by default. A Mac client talks to the published protocol, which already exists; it does
not talk to `kvit-asrd.exe`. Crash isolation and "no Python on this machine" are the reasons
that survive.

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
  (`docs/protocol.md` §2.8). Outbound HTTPS to the speech service **is** in: WinHTTP already
  does it (`normalizer.cpp`), and `--service https://…` is a valid URL.
* **Authentication on the listening side.** Loopback only. Unchanged from §2.8.
* **An API key for the speech service.** `openai_http.py` has `api_key`; this LAN box does not
  ask for one. No `--api-key`, no env, no settings field. Real OpenAI (or anything that wants
  `Authorization: Bearer`) is a later, argued addition, not a silent omission inside the HTTP
  client.
* **A settings UI of its own.** Configuration arrives on the command line from the client that
  launched it, or from flags for someone running it standalone (§4.7).

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

**One in flight, always.** Default `hop_ms` is 1000, so a hop usually fits inside one RTT. If a
hop (or an endpoint, or `stop`) fires while a POST is running, it waits for that POST or
replaces the pending work with the newer buffer — it does not queue a second WinHTTP call.
`cancel` and process teardown call `WinHttpCloseHandle` on the in-flight request; `stop` must
not sit on the HTTP timeout (Python default 30 s) before emitting `stopped`.

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

Masking, ping/pong, close codes and continuation live in L0. After a binary WebSocket message
is reassembled, L1 sees the ordinary 4-byte-length protocol frame. `FrameReader` is not a
WebSocket parser.

---

## 3. The port table

Honest line counts against this tree, and what each row actually costs.

| Source | Lines | Disposition |
|---|---|---|
| `protocol.py` | 1,990 | **Not ported.** `src/protocol.cpp` is the same spec in C++, bidirectional, and the fixtures test it. |
| `transport.py` | 803 | **Not ported.** Named pipes; the translator listens on a socket. |
| `ws.py` | 436 | **Reimplemented in C++** (§4.1). Budget against this file and `minimal_server.py`, not a round 300. The Python file remains the reference and the second implementation. |
| `stability.py` | 874 | **Port the tracker only, ~250 lines.** The grapheme/UTF-8 half is `util/utf8.cpp`. The algorithm is the Python tracker's, not `commit_engine.cpp` run backwards. |
| `vad.py` | 1,207 | **Port `EnergyZcrScorer` + `VadEndpointer`, ~350 lines.** Silero is out of v1 — see §4.4, which is blunt about what that costs. |
| `session.py` + `connection.py` | 1,358 | **Port the routing, ~400 lines.** Much of this is CLI surface and backend-selection machinery a fixed-backend exe does not need. |
| `backends/base.py` `PseudoStreamingBackend` | ~200 of 811 | **Port, ~200 lines.** The overlapping-window driver, including the prefix-freeze on window slide, which is the subtle part. |
| `backends/openai_http.py` | 410 | **Port to WinHTTP, ~350 lines.** Multipart is hand-rolled on both sides; `normalizer.cpp` is the shape to copy. |
| `server.py` | 1,316 | **Port the run loop, ~200 lines.** |

Rough total of new C++: **1,750–2,200 lines**, against roughly 2,200 lines of actually-linked
code (`protocol.cpp` + `utf8.cpp` + `json.cpp` + `log.cpp` and their headers). Do not count
`commit_engine.cpp` or `kd_core` in that second number.

---

## 4. The pieces

### 4.1 The WebSocket server (`src/asrd/ws_server.{h,cpp}`)

The one piece of genuinely new transport code, and the reason is worth stating: **WinHTTP is a
client API.** It gave the *client* its WebSocket for free (`ipc_client.cpp`); there is no
server-side counterpart short of `http.sys`, which means an HTTP namespace reservation and, on a
non-loopback binding, elevation. Neither is acceptable for a program whose installer is
deliberately per-user and prompt-free.

So: raw Winsock (`ws2_32`), and RFC 6455 by hand. Budget **400–500 lines**, against `ws.py` (436)
plus `docs/examples/minimal_server.py` (executed by 27 tests). Ping/pong, close codes, continuation,
and control frames interleaved with data are in that budget — they are L0, the protocol says they
are invisible above it, and omitting them is how a handshake that looks fine dies on the first
idle. The C++ version is a transcription of a design that has been validated against a real
WinHTTP client twice.

What it must get right, all of which the two Python versions already encode:

* `Sec-WebSocket-Accept` = base64(SHA-1(key + `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)). **That
  GUID is written here because getting it wrong from memory cost an afternoon**: both endpoints
  compute a matching-looking handshake and WinHTTP rejects the upgrade with "invalid server
  response". The RFC's own test vector goes in the unit test, not just the constant.
* Echo the `kvit-dictate.v1` subprotocol when offered, never invent it (§2.6.2).
* Reject unmasked client frames (RFC 6455 §5.1). Server frames are sent unmasked.
* Reassemble continuation frames before decoding (§2.6.3).
* Binary only; a text message is fatal `unknown_frame_type` even when its bytes would decode.
* Validate the redundant 4-byte length prefix against the message size (§2.6.3).
* Answer ping with pong; answer a Close; tolerate a client that vanishes without one (§2.6.4).
* Answer a plain HTTP request with a 400 that says what the port is — somebody will paste the URL
  into a browser.

SHA-1 comes from `bcrypt.dll` (`BCryptCreateHash` with `BCRYPT_SHA1_ALGORITHM`), which
`CMakeLists.txt` already links for `BCryptGenRandom`. Base64 is twenty lines by hand in this
file. `crypt32` is not a current dependency and is not added for one call.

### 4.2 The session loop (`src/asrd/session_server.{h,cpp}`)

The server-side mirror of `src/session.cpp`. One session per connection (§7.2), and the state
machine is small because the protocol is:

```
hello          → validate proto; hello_ack (with models[]); then load
                 load fails → fatal error (not ready), orderly close
                 load ok    → ready
start          → open session, reset endpointer and stability, utterance_id = 1
audio (0x02)   → append to the utterance buffer; feed the endpointer
  endpoint     → close utterance, emit final, ++utterance_id
  hop elapsed  → decode window, emit partial  (off in finalize-only)
stop           → flush, decode the remainder, final, stopped
cancel         → drop everything, abort in-flight HTTP, stopped(reason=cancelled),
                 no further partial/final
discontinuity  → flush the decoder's state
```

**Ready is earned.** `OpenAiHttpBackend._load()` TCP-connects to the speech host before the
Python server emits `ready`, so the user is not talking into a dead box. asrd does the same:
`--service` is resolved and connected (a TCP connect, not a transcription) during load. Failure
is a fatal `error` (`connect_refused` or `model_load_failed`) **instead of** `ready`. The Test
button in §4.6 depends on this: a green Test means the translator *and* the speech host answered,
not that a port is open.

Every rule that is easy to get wrong is already written down and already has a fixture:
`session_id` echoed verbatim and never normalised (§7.1); `seq` strictly increasing per session
(§7.4); `stopped` always emitted, cancelled or not (§10.4); `stop` for an unknown session
answered `stopped(reason=unknown_session)` rather than an error (§10.5).

### 4.3 Stability, from the server's side (`src/asrd/stability.{h,cpp}`)

`plan.md` §1.4's six steps. `stability.py` is the **producer** (hypotheses in, `stable_bytes`
out). `commit_engine.cpp` is the **consumer** (that mark is an upper bound; the engine commits
the minimum of its own computation and the server's). The translator ports the producer. It does
not call `CommitEngine`.

The utf8 primitives are the shared implementation of steps 2–4:

1. Keep the last **N** hypotheses for the current utterance (N = 2).
2. Candidate = longest common prefix over those N, **on grapheme clusters** —
   `utf8::common_grapheme_prefix(items, count)`, which already exists and takes an array.
3. Retract at least **M** clusters behind the decode head (M = 3) — `utf8::retract_graphemes`.
4. Retract to the last word boundary and hold back trailing whitespace —
   `utf8::retract_to_word_boundary`, `utf8::trim_trailing_whitespace`.
5. Require the candidate unchanged for **T** ms (T = 200).
6. Never decrease, and never let the bytes below the published mark change.

Step 3 has two readings — subtract M from the common prefix, or cap the candidate at
"M clusters behind the decode head". `stability.py:831-836` already flags this and takes the
second. The C++ tracker takes the second. A property test that disagrees with Python on this
point is a failed port, not a refinement.

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
occasionally — `models.json` records a violation within seconds of the first human run. `kvit-asrd`
therefore defaults **its own command line** to `--finalize-only on`: one `final` at the endpoint,
no `partial`s, `ready.mode = "pseudo-streaming"` (the protocol's taxonomy has no other value for a
batch model behind a window). That stays, because a translator serving an unknown client should
promise the least.

**The client no longer agrees, and the reason this was ever a dilemma was a conflation.**
`Settings::live_preview` now defaults to on, so kvit-dictate launches its own translator with
`--finalize-only off`. Two separate things were one flag:

* **Showing a hypothesis** mid-utterance. This is what the user asked for and what the overlay
  greys out as "still moving". It promises nothing.
* **Publishing a mark** on it. This is a promise under §8.2, and this backend cannot keep it.

They were the same line in `connection.cpp` — `publish_stable_partials = !finalize_only` — which
made turning the preview on also start making promises. A `partial` with `stable_bytes = 0` makes
none: §8.2 constrains only the published prefix, so with none published the model may revise every
byte it has ever emitted and stay conforming, and the `final` need only begin with a zero-length
prefix. `commit_engine.h` already says this is the shape it expects from a revising backend —
"every op for such a backend is produced by `on_final`".

The cost of getting that wrong was not flicker. Measured on 2026-08-28, in real dictation: the
model extended honestly for 490 bytes, earned a mark, then re-punctuated byte 421. §8.3 obliges
the sender to refuse rather than emit, refusing fails the session, and failing the session
discards the utterance in flight — half a minute of speech, gone, with no key pressed. The
tracker was not at fault and no setting of N/M/T would have helped; the mark should never have
been offered. See `asrd_session.live_preview_publishes_no_mark_so_a_late_revision_cannot_take_the_session`.

So preview is on, the mark is off for this backend permanently, and everything still commits at
the `final`. Off remains one checkbox away, still labelled.

**And the window is now as long as an utterance, which retires the stitcher from the shipped
path.** Turning the preview on also woke the sliding window: with `window_ms = 8000` a speaker
who talks for twenty seconds slides it a dozen times, and every slide is stitched by
`scrolled_off_words`, which matches the tail of the previous transcript against the head of the
new one. One revised word inside that overlap — `give` becoming `gives` — makes every candidate
`k` fail, and the fallback freezes the *entire* previous window while the new window still holds
seven of those eight seconds. Reported the same evening as "a dictation that somehow duplicated",
with the seam visible in the text.

Sliding buys bounded decode cost for unbounded audio, and the audio is not unbounded: the
endpointer cuts an utterance at `VadConfig::max_utterance_ms`. So `window_ms` is now read from
that, the window always covers the whole utterance, `slid` is never true, and the preview and the
`final` are the same computation over the same recording. `scrolled_off_words` and its tests
stay — a smaller `--window-ms` would still need them — but nothing ships through them. See
`asrd_backend.the_shipped_window_holds_a_whole_utterance_so_nothing_is_ever_stitched` and, for
the arithmetic, `a_window_shorter_than_the_utterance_duplicates_what_the_model_revises`.

The stability tracker is still built and still runs, because the moment a streaming backend
arrives it is the thing standing between the model and the user's document. T2's gate is a
property test: the C++ tracker and the Python tracker, over the same hypothesis sequences, agree
byte for byte. The fixture corpus does not cover this computation.

**Who owns N/M/T.** The server. `Settings::stability` on the client stays as `CommitEngine`'s
independent second opinion, with its existing compiled defaults, and the Settings dialog does
**not** grow a second copy of those knobs. asrd takes `--stability-n/m/hold-ms` on its command
line (defaults 2/3/200). Two places to *set* them is one too many; two places to *compute*,
with the server's mark as an upper bound, is the architecture `commit_engine.h` already
documents.

### 4.4 Endpointing (`src/asrd/vad.{h,cpp}`)

Port `EnergyZcrScorer` and `VadEndpointer` — frame energy in dBFS plus zero-crossing rate, a
logistic on the pair, hysteresis into speech/silence states, and an utterance boundary after
`endpoint_silence_ms` (default 1500 for this backend).

**Silero is out of v1, and `release-plan.md` §1.6 already established what that costs — which is
nothing, because it was never running.** `config.py` `--vad` defaults to `auto` (the flag is at
line 330, not 248), `--silero-model` defaults to `None`, no `models.json` entry sets it, and
`SileroOnnxScorer` imports `onnxruntime` inside `load()`, so on every run to date it has failed
to load and fallen back to the energy scorer. **That is what has been endpointing every
dictation this project has ever done.** Porting the energy scorer is therefore not a downgrade;
it is porting the thing that is actually in use. Its own file calls it "not a substitute for
Silero" and that remains true and remains a §10 item.

Separately and confusingly, `models.json` sets `vad_gate=true`, which is a *different* mechanism:
a call to the **server's** Silero over `/v1/audio/vad`. That one is ported (§4.5) because it is
doing real work — it is what stops 500 ms of silence being transcribed as "Yeah."

### 4.5 The OpenAI-compatible backend (`src/asrd/backend_openai.{h,cpp}`)

`POST {base_url}/v1/audio/transcriptions`, `multipart/form-data`, fields `model`, `language`,
`response_format=json`, plus a `file` part holding the utterance as a WAV. Reply is JSON with a
`text` field. Default model `Qwen/Qwen3-ASR-1.7B`, default language `en` — the values
`openai_http.py` already uses against this host.

* **WAV, not a compressed format.** The standard library can write it, it is lossless, and
  re-encoding would put a codec between the measured audio and the model for no benefit.
* **Multipart by hand**, as in Python, with a fixed boundary. A WAV cannot contain the token
  because its body is length-prefixed rather than delimited.
* **WinHTTP**, following `normalizer.cpp`: synchronous on a worker, per-operation timeouts,
  `http://` and `https://`, cancellation via `WinHttpCloseHandle`. A private helper in
  `src/asrd/`, not a call into `Normalizer`.
* **Load-time connect**, as in Python `_load()`: a TCP connect to the host:port, so a down box
  fails before `ready` rather than mid-utterance.
* **The VAD gate**, optional and on by default for this backend: `POST /v1/audio/vad` first, skip
  the transcription when it says no. **It fails open** — a missing route (503), a timeout, a
  malformed answer all mean "go ahead". Losing something the user actually said is worse than an
  occasional invented word, and a VAD outage must never be able to silence dictation.
* **The pseudo-streaming window**, ported from `PseudoStreamingBackend`: re-transcribe the whole
  window every `hop_ms` and emit the result as the cumulative hypothesis. Past `window_ms` the
  oldest audio scrolls out and its transcript must be **frozen** into a prefix, estimated by
  word-overlap stitching — which can be wrong, in ways `base.py` documents honestly and which
  must be copied across with the code rather than left behind with it. Finalize-only makes
  `hop_ms >= window_ms`, so the window never slides; that is the v1 default.

### 4.6 Configuration, Settings, and how it is started

This is the part the whole document exists for. **The user configures one address.**

#### Settings schema

`ServerSettings.url` today is a protocol WebSocket. `parse_server_url` treats `http://` and
`https://` as **WebSocket synonyms**. Storing `http://strixhalo.lan:10301` in that field would
make the client attempt a WinHTTP upgrade against the OpenAI HTTP service — the failure this
file exists to end.

Do **not** bump `kSettingsSchema`. Add an optional `server.mode`:

| `mode` | Meaning of `server.url` | Parser |
|---|---|---|
| `"local"` (default for empty / new) | OpenAI-compatible speech service | `http://` or `https://` only. A separate helper, not `parse_server_url`. |
| `"protocol"` | A `docs/protocol.md` server | existing `parse_server_url` (`ws://`, `wss://`, `http://`/`https://` as synonyms) |

Migration of an existing `settings.json` that has no `mode`:

* `url` empty → `local`, first-run Server page (unchanged empty-URL behaviour).
* `url` starts with `ws://` or `wss://` → `protocol`. A user who already pointed the app at a
  Python server keeps that.
* anything else → `local`, and the URL is re-validated as a speech-service address on next
  save. Do not silently feed it to `IpcClient::connect`.

The Settings → Server page:

```
Speech service   [ http://strixhalo.lan:10301        ]   ← the only thing most people type
                 ( ) I run my own kvit-dictate server → ws://...
Model (optional) [                                   ]
                 [Test]
```

Local is the default selection. The protocol-server radio reveals the existing `ws://` box.
The model field is `hello.model` in both modes; empty means the server's default.

**Test** always speaks the protocol to whatever this process will actually connect to.

* `protocol` mode: today's `probe_server` against `server.url`. Unchanged.
* `local` mode: spawn `kvit-asrd.exe` if it is not already running, handshake **it**, wait for
  `ready`. A Test that POSTs `/v1/audio/transcriptions` directly would pass while the translator
  is broken. Because asrd does not emit `ready` unless the speech host connected, a green Test
  is the whole path.

The chosen loopback port is logged and shown on this page while the translator is up.

#### Spawn — a new path, not the old one re-enabled

In local mode the client **launches `kvit-asrd.exe` from its own directory**, on loopback, on a
port the client chose, with the service URL on a command line this program constructs, and
supervises it for its lifetime.

**Yes, this reinstates process spawning that `release-plan.md` §1.2 deleted, and the distinction
is the whole point.** What §1.2 removed was spawning *an arbitrary interpreter named in a
user-editable `models.json`, with a user-supplied argv and environment*. What this adds back is
launching **one fixed executable, by absolute path, from the installation directory**. The first
is a configuration file that can execute anything; the second is two halves of one application.

Concretely:

* **`KD_ENABLE_SIDECAR_SPAWN` stays off in shipped builds.** The named-pipe / `models.json` /
  interpreter path is not re-enabled. The `#ifdef` around `ChildSpec` and `IpcClient::start`
  stays.
* **Translator spawn is always compiled into the client.** It is not behind that flag. A shipped
  `kvit-dictate.exe` **will** contain `CreateProcessW` and `AssignProcessToJobObject`. That is
  a reversal of the "attack surface a binary does not contain" sentence in
  `packaging/windows/build-release.ps1`, and it is accepted because the alternative is the
  Python window. The security property that remains is: the image path is
  `GetModuleFileName(nullptr)`'s directory plus `kvit-asrd.exe`, and nothing else; if that file
  is missing, the tray says so and nothing is launched.
* **`build-release.ps1` drops the import-table grep for `CreateProcessW` /
  `AssignProcessToJobObject`.** Replace it with: (1) `KD_ENABLE_SIDECAR_SPAWN=OFF` still passed
  to CMake, (2) `kvit-asrd.exe` is present in the stage directory, (3) a strings check that the
  staged client does **not** contain the sidecar log line `"spawned sidecar pid"` (the unique
  marker of the old path). `release-plan.md` §1.2's gate table is updated in the same change.
* **Job object.** Same discipline as the pipe spawn: create the job first,
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, assign the child before it runs. Nested jobs work on
  this machine (Windows 8+); `ipc_client.cpp:28-29` already measured that
  `AssignProcessToJobObject` succeeds when this process is itself inside a job, and that
  closing the job handle kills the child. Carry the measurement, do not re-discover it.
* **Argv.** Constructed by the client, quoted. The user's speech-service URL is data on that
  command line — it is user-supplied *content*, not a user-supplied argv vector. Validate it as
  `http://` or `https://` (no spaces, no quotes, no control characters) before quoting it onto
  `--service`. No environment-block mapping. No `models.json`. Working directory = the
  installation directory.
* **`--parent-pid`.** The parent always passes its own pid. asrd watches it and exits if it
  dies; the job object is the real guarantee, the watch is backup for the case the child is
  somehow not in the job. `kd_probe` and standalone use omit it; asrd does not require it.

#### Port

**The client chooses the port.** Bind `127.0.0.1:0`, `getsockname`, close, pass
`--listen 127.0.0.1:N`. Small TOCTOU on loopback; if the child exits because the bind failed,
retry a handful of times with a fresh port. Do not use a fixed `10402` for the supervised case
— two clients, or a leftover Python server, would collide.

Standalone still takes an explicit `--listen` (default `127.0.0.1:10402` if omitted, so
`kd_probe --server ws://127.0.0.1:10402/` against a hand-started asrd keeps working).

The chosen port is logged (`KD_LOG_INFO`) and shown in Settings while the child is up.

#### Standalone

```
kvit-asrd.exe --listen 127.0.0.1:10402 --service http://strixhalo.lan:10301 [--model ...]
```

That is also how the translator runs on another machine. `--listen` on a non-loopback address
is an explicit act; the default is loopback. There is still no auth and no TLS on the listen
side (§1.4).

### 4.7 Command line

| Flag | Required | Default | Notes |
|---|---|---|---|
| `--listen [host:]port` | standalone; parent always passes it | `127.0.0.1:10402` | `ws.py` / Python `--listen` shape. Bind loopback unless the host is given. |
| `--service URL` | yes | none | `http://` or `https://` only. |
| `--model NAME` | no | `Qwen/Qwen3-ASR-1.7B` | `hello.model` / `ready.model`. |
| `--language LANG` | no | `en` | POST field. Empty string means omit the field (server default). |
| `--parent-pid PID` | no | unset | Watch and exit if that process dies. Parent always passes; `kd_probe` omits. |
| `--max-connections N` | no | `1` | `0` = unlimited. Extras get `server_busy` after `hello`. |
| `--finalize-only` | no | **on** | Off is live preview, labelled. |
| `--vad-gate` | no | **on** | Fail-open. |
| `--endpoint-silence-ms` | no | `1500` | |
| `--stability-n` / `--stability-m` / `--stability-hold-ms` | no | `2` / `3` / `200` | |
| `--hostile-stability` | no | off | Same name and contract as Python. Logged at ERROR. Not reachable from backend options. |
| `--log-level` | no | `info` | |

No `--api-key`. No `--silero-model`. No `--backend`.

### 4.8 Build: CMake, tests, packaging

`kd_core` is a single static lib that includes overlay, tray, injector, WASAPI and the settings
dialog. **`kvit-asrd` does not link `kd_core`.** Linking it would pull Direct2D into a headless
translator and make the size claim a lie.

New targets:

```
kd_wire          STATIC     protocol.cpp, util/utf8.cpp, util/json.cpp, util/log.cpp
                            (headers: those plus clock.h, result.h)
                            kd_core and kvit-asrd both link this; kd_core's source list
                            loses the files that moved.

kvit-asrd        CONSOLE    src/asrd/main.cpp
                            src/asrd/ws_server.cpp
                            src/asrd/session_server.cpp
                            src/asrd/stability.cpp
                            src/asrd/vad.cpp
                            src/asrd/backend_openai.cpp
                            src/asrd/http.cpp          WinHTTP helper, not Normalizer
                            links: kd_wire, kd_common_flags,
                                   bcrypt, winhttp, ws2_32
                            /MT, asInvoker, same version macros as the client.
                            No icon, no application manifest beyond asInvoker.
                            Console subsystem: standalone use is a terminal; when the
                            client spawns it, CREATE_NO_WINDOW hides the console.
```

`kd_tests` keeps linking `kd_core` (and therefore `kd_wire`). The in-process loopback test
in §5 compiles the asrd sources into `kd_tests` the same way overlay tests compile overlay —
or links `kvit-asrd`'s objects. Either way the asrd code is in the test binary; it is not
driven only through `CreateProcess`.

T0 is the CMake change. Without `kd_wire` and `add_executable(kvit-asrd …)`, T0 is a handshake
stub with nowhere to live.

**Logging.** asrd uses `kd::log` to stderr, module tag `asrd`. Recognised text is not a log
line (`log.h`'s rule applies here too). Supervised: the parent redirects the child's stderr
into the client's log, same as the existing sidecar stdout capture. Standalone: the console.
No separate `asrd.log` unless a `--log-file` is added later; the first support question is
answered from the client's log.

**Packaging.** `build-release.ps1` stages `kvit-asrd.exe` next to `kvit-dictate.exe`. The Inno
script already copies `{#StageDir}\*` with `recursesubdirs`, so there is no new `[Files]`
entry — the staging step is the change. The ZIP grows; still one download, still no Python,
still no runtime to install. Same version number on both binaries.

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
  transcription (must be `decode_failed`, non-fatal, session aborted), a non-JSON body, a
  timeout, and a down host at load (must not emit `ready`).
* **Stability property tests.** Same hypothesis sequences through the C++ tracker and the
  Python tracker; byte-for-byte agreement on `stable_bytes`. This is the T2 gate. It is not
  optional and it is not covered by the fixture corpus.
* **Hostile stability.** `server/tests/test_hostile_stability.py` already exists to force illegal
  `stable_bytes` onto the wire so §8.3 can be exercised end to end. The C++ translator needs the
  same switch, with the same name and the same loud logging, and it must not be reachable from
  backend options — a backend must never be able to authorise its own protocol violations.
* **Spawn path.** Absolute path only; a missing `kvit-asrd.exe` is a tray error and not a
  `CreateProcess` of anything else. Job-close kills the child. `--service` with a space or a
  quote is rejected before it reaches argv.

## 6. Packaging

Covered in §4.8. The short version: two executables in the stage directory, `/MT` on both,
manifest only on the client, Inno already recursive, release script's `CreateProcessW` grep
replaced by the narrower checks in §4.6.

---

## 7. Phases

| Phase | Work | Gate | Status |
|---|---|---|---|
| **T0** | CMake: `kd_wire` + `kvit-asrd`. `ws_server` + the fixture corpus in the server role; a translator that completes a handshake and answers `ready` with a stub backend | `kd_probe --server ws://127.0.0.1:10402/` against a standalone `kvit-asrd.exe`, and the 21 fixtures green in both roles | **Done.** `kd_probe` against a standalone `kvit-asrd.exe` pointed at `http://strixhalo.lan:10301`: handshake 11 ms, ready 35 ms. Fixtures green in both roles — see `tests/test_asrd_fixtures.cpp`, which is explicit about what "the other role" can honestly mean for each kind. |
| **T1** | Session loop, endpointing, the OpenAI backend in finalize-only mode, load-time connect, one-in-flight HTTP, cancel-aborts-POST | A full dictation into Notepad, through `kvit-asrd.exe` started by hand, against strixhalo, with no Python running anywhere. Observed, not asserted. | **Done, with the gate half-met by construction.** The dictating half needs a human at a microphone. The rest is `tests/test_asrd_session.cpp`: the real `IpcClient` against the real translator over a loopback socket, plus an opt-in case (`KD_ASRD_SERVICE`) that completes the whole path against strixhalo and gets a conforming `final` back. |
| **T2** | Stability tracker + pseudo-streaming window; live preview available and off by default | Property tests green (C++ tracker == Python tracker on `stable_bytes`). Live preview against a batch model produces either a conforming stream or a refused emission — never a `stability_violation` on the client | **Done.** 17 recorded cases in `tests/fixtures/stability/`, agreeing byte for byte, step 3's disputed reading included. The live-preview gate is `asrd_session.live_preview_against_a_revising_model_never_reaches_the_client_broken`, and what it finds is worth knowing: with N=2 a model that recases the first word collapses the common prefix to zero, so nothing is ever published and everything commits at the `final`. Exactly what `publish_stable_partials` predicts. |
| **T3** | Translator spawn (always compiled); `build-release.ps1` grep rewrite; Settings → Server becomes one address (`server.mode`, Test talks to asrd); `docs/setup.md` and README teach the speech-service URL | Install from the installer on a machine with no Python, type `http://…:10301`, press Test, dictate. Existing `ws://` settings still open a protocol connection and do not spawn asrd | **Done, less the clean-machine install.** The release script produces an installer with both halves and its new gates pass. The spawn is verified against strixhalo by an opt-in test that launches a real child, gets `ready` in 46 ms, and confirms the job object killed it. The migration is unit-tested for every case; the author's own `settings.json`, which has a `ws://` URL and no mode, reads as `protocol` and spawns nothing. **A clean-VM install is still owed** — `release-plan.md` §3's gate, unchanged by this work. |
| **T4** | Retire `run-server.bat`; delete the instructions that told the user to keep a Python window open | The file is gone, the docs do not mention it, and nothing in the shipped tree needs it | **Done.** |

T0 does not include spawn. T3 does not start until §4.6's spawn contract and settings schema are
the code, not just this file.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| The pseudo-streaming freeze-on-slide heuristic mis-stitches and commits a wrong prefix | Inherited from `base.py` along with its documented failure cases. v1 defaults to finalize-only, where the window never slides because the utterance ends first. |
| Porting `stability.py` introduces a divergence the fixtures do not catch | T2 property tests, byte for byte, including the step-3 reading. `commit_engine.cpp` is not the oracle. |
| Hand-rolled RFC 6455 on the server side has a bug the Python version does not | It is written third, against two working implementations and a corpus, and tested against a real WinHTTP client rather than against itself. Budget includes ping/pong and close. |
| Spawning a child undoes §1.2's attack-surface reduction | The old path stays compiled out. The new path launches one fixed image from the install directory. The release grep is rewritten to match that fact rather than pretending `CreateProcessW` is absent. |
| The energy endpointer is worse than Silero and now it is the only one | It is already the only one, and has been for every dictation this project has run. Stated, not discovered. Silero is a §10 item. |
| Two binaries doubles the signing work when a certificate exists | True and cheap; both are signed by the same script. |
| Client-chosen port TOCTOU | Loopback, short window, retry on listen failure. Logged. |
| An existing `ws://` settings file is reinterpreted as a speech URL | Migration in §4.6: `ws://`/`wss://` ⇒ `mode=protocol`. Tested in T3. |

---

## 9. What this does not change

`docs/protocol.md` is untouched. No new message, no new field, no proto bump. That is the test of
whether the boundary was drawn in the right place, and it passes: a whole new server, in a
different language, with a different backend, and the spec does not move.

`server/` keeps working, unchanged, for the bench and for every backend that needs weights.
`--listen` stays. The Python server remains the reference implementation for anything
`kvit-asrd` does not do, which for v1 is most things.

`KD_ENABLE_SIDECAR_SPAWN` still means what it says on the tin: the named-pipe interpreter spawn.
Shipped builds still turn it off. They gain a different, narrower spawn.

---

## 10. Open questions

Closed in this revision, so they are not re-litigated in T2/T3:

* **N/M/T UI.** Server owns the published mark. Client `CommitEngine` keeps its defaults. No
  second settings page. (§4.3)
* **Ephemeral vs fixed port.** Client-chosen port for the supervised case; `--listen` default
  `127.0.0.1:10402` for standalone. (§4.6)

Still open:

1. **Silero in the translator.** ONNX Runtime is a real dependency and `CMakeLists.txt` forbids
   third-party libraries outright. Reaching the server's `/v1/audio/vad` covers the case that
   matters today. Revisit when a local backend arrives, since that breaks the rule anyway.
2. **Cross-platform later.** Nothing here forecloses a Go or Rust translator against the same
   protocol, and §1.3's cross-compilation argument is the one real thing C++ gives up. If the
   translator ever wants to live on the ASR box rather than the desktop, that is the moment to
   revisit — not now, and not by rewriting this one.
3. **Does `models.json` survive at all?** It is already gone from shipped builds. Once the bench
   is the only consumer, it may belong under `bench/` rather than at the repository root.
4. **API key, later.** The Python backend has one. This LAN box does not. When someone points
   `--service` at a host that wants a bearer token, that is a settings field and a header, and
   it is a separate argument because it is a secret on a command line.
