# Standing up a server

kvit-dictate does not recognise speech. It captures your voice, sends it to a server you run, and
types back what comes out. **This is the part the project does not do for you yet**, and this page
is how you do it.

That is a real cost and worth stating plainly before you spend an evening on it: `release-plan.md`
§8 calls the first-run cliff — "install this, then go set up a server" — the single biggest
adoption risk in the project, and it is a deliberate choice rather than an oversight. A reference
server that makes this one download and one `run.bat` is a separate deliverable on its own
timeline (`release-plan.md` §1.5). Until it exists, the audience for kvit-dictate is people who
already run, or are willing to run, an ASR server.

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

Put `ws://127.0.0.1:10402/` into kvit-dictate's **Settings → Server** and press **Test**. You
should see the server's build id, a model called `counter`, and a round-trip time in
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

This is the shortest path from "the plumbing works" to "a real model wrote that". The
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
| Server address | `ws://127.0.0.1:10402/` for this machine, `ws://box.lan:10402/` for another. `wss://` and `https://` work too. |
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

### If Test fails

The message names which of four things happened, because each has a different fix:

| Message | What it means |
|---|---|
| `connect_timeout` | Nothing answered. Wrong address, machine off, or a firewall dropping the packets. |
| `connect_refused` | Something answered and said no. The server is not running, or not on that port. |
| `tls_failed` | `wss://`, and this machine does not trust the certificate. |
| `answered HTTP 200 instead of upgrading` | The URL points at an HTTP API rather than at the WebSocket endpoint. Check the path. |
| `server_busy` | The server is already serving someone. `--max-connections` on the server side, or wait. |
| `model_unavailable` | You named a model it does not have. The message lists what it does have. |

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

## 6. Running it as a service

Out of scope for this page, and the honest reason is that the answer differs per platform and none
of them has been tested here. On Windows, NSSM or a scheduled task at logon both work. On Linux, a
`systemd` user unit. The server is a normal foreground process that writes one JSON object per
line to stderr; anything that can supervise that will do.
