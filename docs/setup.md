# Pointing kvit-dictate at a speech service

kvit-dictate does not recognise speech. It captures your voice, sends it somewhere, and types
back what comes out. This page is about the "somewhere".

There are **two** things it can talk to, and which one you have decides how much of this page
applies to you:

| You have | Settings → Server | What you run |
|---|---|---|
| An **OpenAI-compatible speech service** — anything serving `/v1/audio/transcriptions` | Type its address. Leave the first radio selected. | Nothing. kvit-dictate starts and supervises `kvit-asrd.exe` for you. |
| A **kvit-dictate protocol server** — `server/` from this repository, or your own | Type its `ws://` address and pick "I run my own kvit-dictate server". | That server. |

**If you have the first, you are two minutes from working and the rest of this page is
background.** Sections 1-3 are about standing up a protocol server, which you do not need.

---

## 0. The short version: one address

If something on your network already serves `/v1/audio/transcriptions` — a llama.cpp-style ASR
server, a Qwen3-ASR box, a vLLM deployment, anything OpenAI-compatible — then:

1. Open **Settings → Server**.
2. Type its address into **Speech service**: for example `http://strixhalo.lan:10301`.
3. Press **Test**.

Test does not merely ping that address. It starts `kvit-asrd.exe` — the translator that ships
beside `kvit-dictate.exe` — hands it your address, and completes the full protocol handshake with
it, exactly as dictation will. `kvit-asrd` does not report itself ready until it has connected to
your speech service, so a green Test means the whole path answered: this client, the translator,
and your service. A test that POSTed to your service directly would pass while the translator was
broken, which is not a test worth having.

Then dictate. There is nothing else to run, no window to keep open, and no Python.

### What the translator is

`kvit-asrd.exe` is the second half of the application, installed beside the client. It speaks
`docs/protocol.md` on one side and HTTP on the other:

```
kvit-dictate.exe  ──ws──▶  kvit-asrd.exe  ──HTTP──▶  your speech service
   hotkey, capture,        endpointing,               /v1/audio/transcriptions
   injection, overlay      stability, WAV
```

The client launches it on a loopback port it chooses, supervises it, and kills it when the client
exits — including when the client crashes, because the child is in a job object. You never start
it yourself unless you want to (§5).

**What it does not do yet:** no API key. If your service wants an `Authorization: Bearer` header,
this is not yet the way to reach it — that is a settings field and a header, and it is a separate
argued change rather than a silent omission inside the HTTP client
(`openai-translator.md` §1.4, §10 item 4).

---

## 0.1 If you do not have a speech service

Then you are standing one up, and the rest of this page is for you. Be warned that it is a real
evening's work: `release-plan.md` §8 calls the first-run cliff the single biggest adoption risk in
the project, and it is a deliberate choice rather than an oversight.

Everything below assumes you have the repository, or at least `server/`. If you only have the
installer, start at [§4](#4-putting-a-server-you-already-run-behind-the-protocol).

---

## 1. The five-minute version: prove the plumbing first

Before a model, before a GPU, before anything that can go wrong for an interesting reason, check
that the client can reach a server at all. `docs/examples/minimal_server.py` is a complete
conforming server in the standard library that **counts audio instead of transcribing it**:

```
python docs/examples/minimal_server.py
listening on ws://127.0.0.1:10402/
```

Put `ws://127.0.0.1:10402/` into kvit-dictate's **Settings → Server**, pick **"I run my own
kvit-dictate server"**, and press **Test**. (The radio matters: with the first one selected, that
box means a speech service and kvit-dictate will refuse a `ws://` address with a sentence saying
so.) You should see the server's build id, a model called `counter`, and a round-trip time in
milliseconds. Dictate into Notepad and it will type

```
heard 2.4 s of audio (38400 samples)
```

which is not speech recognition, and is exactly the point: the hotkey, the microphone, the
network, the framing, the anchor and the injector have all just been proved end to end. If this
does not work, nothing further down this page will either, and you now know which half to look at.

Anything else that goes wrong from here is a model problem.

---

## 2. A real server, from this repository

`server/` in this repository is the ASR sidecar. It speaks the protocol natively — it is one half
of the conformance suite — and `--listen` is what makes it a network server rather than a child
process.

### 2.1 The mock, over the network

The same shape as §1, but with the real server code:

```
cd server
py -3.12 -m kvit_asr --listen 127.0.0.1:10402 --backend mock
```

The mock types scripted fixture sentences. It is for testing the client, not for dictating.

### 2.2 Against an OpenAI-compatible transcription endpoint

**You almost certainly want [§0](#0-the-short-version-one-address) instead.** `kvit-asrd.exe`
does exactly this, ships with the app, needs no Python at all, and is what the first radio in
Settings configures. What follows is the Python equivalent, which is still here because the bench
runs it and because it is the reference implementation of everything `kvit-asrd` was ported from.

The
`openai-http` backend posts audio to any endpoint that implements
`POST /v1/audio/transcriptions` — which is what vLLM, llama.cpp's server, faster-whisper-server,
Speaches and several others already expose. **It needs nothing installed**: the backend module is
`urllib` and `wave` and runs on a bare CPython 3.12.

```
cd server
py -3.12 -m kvit_asr --listen 127.0.0.1:10402 ^
    --backend openai-http ^
    --model Qwen/Qwen3-ASR-1.7B ^
    --backend-opt base_url=http://your-gpu-box.lan:10301 ^
    --backend-opt finalize_only=true ^
    --backend-opt vad_gate=true ^
    --endpoint-silence-ms 1500
```

Three of those flags are not decoration:

* **`finalize_only=true`.** A batch model re-punctuates earlier words as later context arrives,
  which violates the committed-prefix invariant *structurally* rather than occasionally — it
  produced a violation within seconds of its first human run. With this flag the server emits one
  `final` at the endpoint and no `partial`s, so there is nothing to revise. Live preview against a
  batch model is available and is labelled for what it is; see `models.json` for the long version.
* **`vad_gate=true`.** Batch endpoints answer 500 ms of silence with an invented word — this one
  reliably says "Yeah." Gating on the server's own VAD is what stops silence becoming text.
* **`--endpoint-silence-ms 1500`.** How long a pause ends an utterance. The default is tuned for
  a streaming model; a batch one wants longer.

### 2.3 A model on this machine

The other backends — `sherpa-onnx`, `zipformer-hotwords`, `whisper-transformers`,
`faster-whisper`, `windows-speech` — each need their own environment and, in most cases, model
files on disk. **Nothing is ever downloaded automatically** (`plan.md` §1.9). `server/README.md`
§6 lists the requirement file for each, and `bench/CANDIDATES.md` says which models are real,
what they need, and which four are worth trying first.

Once an environment exists, the shape is the same:

```
<that environment's python> -m kvit_asr --listen 127.0.0.1:10402 --backend sherpa-onnx --model-dir <dir>
```

---

## 3. Pointing the app at it

**Settings → Server**, which is the first tab and the one a fresh install opens on.

| Field | What to put in it |
|---|---|
| Which radio | **"…OpenAI-compatible speech service"** for §0's case, **"I run my own kvit-dictate server"** for everything else on this page. It is a stored setting, not a guess from the scheme: `http://` is a legal WebSocket synonym (`docs/protocol.md` §2.6.1), so there is no way to tell the two apart by looking at the address. |
| Speech service (first radio) | `http://strixhalo.lan:10301` — an OpenAI-compatible service. `http://` or `https://` only. |
| Server address (second radio) | `ws://127.0.0.1:10402/` for this machine, `ws://box.lan:10402/` for another. `wss://` and `https://` work too. |
| Model | Leave empty unless your server hosts several. If you name one it does not have, it says so — it never quietly substitutes another. |

Then press **Test**. It opens a real connection, completes the handshake, waits for the server to
report itself ready, and prints what came back: the build id, the model and its streaming mode,
whether it accepts vocabulary hints, the round-trip times, and the model list if the server
advertises one. A cold model load can take minutes and the Test button waits for it — that is not
a hang.

The same check from a command line, which is the useful thing to paste into a bug report:

```
kd_probe --server ws://127.0.0.1:10402/
```

### If Test fails in speech-service mode

Two things can fail, and the message says which:

| Message | What it means |
|---|---|
| `Could not start the translator.` | `kvit-asrd.exe` is not beside `kvit-dictate.exe`. The message names the absolute path it looked at. Reinstall, or use the second radio and run your own server. |
| `The translator started but did not become ready` | The translator is fine and your speech service is not: it could not open a TCP connection to the address you gave. Wrong host, wrong port, machine off, or a firewall. The translator's own diagnostics are in this app's log, on lines prefixed `asrd:`. |

The port the translator is listening on is in the log too, on the `kvit-asrd pid … listening on
ws://127.0.0.1:…` line. It is a fresh port each run — the client picks one so that two clients, or
a leftover Python server on 10402, cannot collide.

### If Test fails against your own protocol server

The message names which of four things happened, because each has a different fix:

| Message | What it means |
|---|---|
| `connect_timeout` | Nothing answered. Wrong address, machine off, or a firewall dropping the packets. |
| `connect_refused` | Something answered and said no. The server is not running, or not on that port. |
| `tls_failed` | `wss://`, and this machine does not trust the certificate. |
| `answered HTTP 200 instead of upgrading` | The URL points at an HTTP API rather than at the WebSocket endpoint. If that HTTP API is your speech service, you want the **first** radio, not this one. |
| `server_busy` | The server is already serving someone. `--max-connections` on the server side, or wait. |
| `model_unavailable` | You named a model it does not have. The message lists what it does have. |

---

## 3.1 Cleaning the text up afterwards (optional, off by default)

**Settings → Server → When you finish → "Clean the text up before typing it in."** One tick box.
When a session ends, what is in the pill goes to a small text model that writes it as prose:
fillers dropped, false starts resolved to whatever you landed on, punctuation and capitalisation
applied, spoken numbers and dates and email addresses in written form. `docs/normalizer.md` is
the whole story.

There is no address to type. The route is your speech service with `/v1/text/normalize` on the
end, derived from the box above it, and it follows that box if you move the service. The tick box
is greyed, with the reason under it, when there is nothing to derive an address from
(second-radio `ws://` mode) or when **Settings → Dictation → Cadence** is not `end-of-session` —
a rewrite can change a word you have already said, and under any other cadence that word is
already in your document.

Nothing about it can cost you a word. A service that is missing, slow or broken leaves the raw
text exactly as it was and logs why, and so does a rewrite that drops one of your **vocabulary
terms**, comes back empty, or comes back suspiciously shorter than what you said. The rewrite
lands in the pill where you can see it, edit it, or throw it away with Esc.

### The five fields with no controls

They have defaults that suit the reference deployment, and they are in the `normalizer` block of
`%LOCALAPPDATA%\kvit-dictate\settings.json` for the deployment that does not. Put them on the
page and every one of them stands between you and the only question you actually have.

| Field | Default | What it is |
|---|---|---|
| `url` | derived | Set it by hand and it is yours: the dialog then never overwrites it, and the status line says it is in use. This is how you run the text model on a different machine from the recogniser. |
| `styling` | `semi-formal` | `casual`, `semi-formal`, `formal`. |
| `structure` | `prose` | `prose`, or `lists` to have it broken into bullet points. |
| `context` | `general` | `general`, or `email`. |
| `timeout_ms` | `3000` | The whole request. Bounded well inside the finalise grace, so a service that has stopped answering can never be the reason a session fails to end. |
| `max_bytes` | `4000` | The longest buffer worth sending. Past it the text is left raw and the log says so; the model's card asks for chunking past about a thousand tokens and this does not chunk. |

These are the endpoint's own trained control values — it answers anything else with 400 rather
than guessing, and the settings file is refused at load with the field named.

---

## 4. Putting a server you already run behind the protocol

The protocol is published (`docs/protocol.md`, Apache-2.0) precisely so this is possible without
talking to anybody. What a conforming server has to do:

1. Accept a WebSocket connection, one protocol frame per binary message (§2.6).
2. Answer `hello` with `hello_ack`, then `ready` (§5).
3. Accept `start`, then 16 kHz mono s16le audio frames, then `stop` (§6).
4. Emit `partial` and/or `final`, and always `stopped` (§6.2).
5. Never let `stable_bytes` go backwards, and never revise bytes below it (§8). If your model
   cannot promise that — and a batch model cannot — emit one `final` at the endpoint, no
   `partial`s, and say `"mode": "pseudo-streaming"` in `ready`. A client that is told will adapt
   its UI instead of showing text that rewrites itself.

`docs/protocol.md` §15 walks through a complete implementation of exactly that, in about 400
lines of standard library, and §15.2 lists the five things implementers get wrong. The file is
`docs/examples/minimal_server.py`; replacing its `transcribe()` with a decoder is the easy half.

---

## 5. Security, said once and plainly

**There is no authentication and no encryption in v1.** Anyone who can reach the port can open a
session and send audio; over `ws://`, anyone on the path can read yours. `docs/protocol.md` §2.8
is the long version.

So:

* **Bind to `127.0.0.1` unless you mean otherwise.** `--listen 127.0.0.1:10402`, not `:10402`.
  The server logs a warning when you bind something routable, and the client's settings page shows
  one when you point it at a host that is neither loopback nor a private address.
* **Run it on a network you control.** That is the supported deployment, and it is the whole of
  the security model.
* **If you need `wss://`, put a reverse proxy in front.** Caddy, nginx or Traefik terminating TLS
  in front of `ws://127.0.0.1:10402` is one configuration file, and it keeps certificate handling
  out of the audio path. The client accepts `wss://` and validates the chain normally.

A firewall rule is worth adding either way: the server is a Python process listening on a port,
and Windows will offer to allow it through on first run. Say no if it is only ever going to serve
this machine.

---

## 5.1 Running the translator by hand

You never have to, but it is the same program and it takes a command line:

```
kvit-asrd.exe --listen 127.0.0.1:10402 --service http://strixhalo.lan:10301
```

That is also how you run it on **another machine** — put it next to your speech service and point
kvit-dictate at it with the second radio, as a protocol server. `--listen` on a non-loopback
address is an explicit act; the default is loopback, and there is still no authentication and no
TLS on the listening side (§5 applies unchanged).

`kvit-asrd --help` lists the rest. The ones worth knowing:

| Flag | Default | What it is for |
|---|---|---|
| `--model NAME` | `Qwen/Qwen3-ASR-1.7B` | The model your service should use. |
| `--language LANG` | `en` | Passed through. Empty means the service's default. |
| `--finalize-only on\|off` | `on` | Off is live preview. See below. |
| `--vad-gate on\|off` | `on` | Ask the service's `/v1/audio/vad` route before transcribing. Fails open. |
| `--endpoint-silence-ms MS` | `1500` | How long a pause ends a phrase. |
| `--log-level LEVEL` | `info` | Diagnostics go to stderr. |

### Live preview, and why the two defaults differ

A batch model does not extend its previous guess, it replaces it. Once it can hear where the
sentence ended it re-cases the first word and inserts the punctuation it was not sure of, so
`so I was thinking we` becomes `So I was thinking, we`. That revises byte 0 — and the protocol
binds each new hypothesis against the mark already published (`docs/protocol.md` §8.2), so once
anything has been committed, no honest answer exists for the next message.

So the translator never tells the client that any of a preview is settled: against this backend
every `partial` carries `stable_bytes = 0`, which promises nothing and therefore cannot be
contradicted by the next one. The whole phrase lands at the endpoint either way. Live preview
does not change what is typed or when. What it changes is what you see while you are talking:
with it on, words appear in the overlay greyed while they are still moving and go solid once they
settle; with it off, you speak into silence until you pause.

That is why the two defaults differ, and neither is an oversight:

* **`kvit-asrd` on its own command line defaults to `--finalize-only on`.** A translator serving
  a client it knows nothing about should promise the least it can.
* **kvit-dictate turns it on** for the translator it launches itself, because it is the thing
  drawing the overlay and it knows the feedback is the point. The checkbox is
  *Settings → Server → "Show words as they are recognised"*, still labelled experimental, and
  turning it off gives you the finalize-only behaviour described above.

---

## 6. Running it as a service

Out of scope for this page, and the honest reason is that the answer differs per platform and none
of them has been tested here. On Windows, NSSM or a scheduled task at logon both work. On Linux, a
`systemd` user unit. The server is a normal foreground process that writes one JSON object per
line to stderr; anything that can supervise that will do.
