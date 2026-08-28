# `server/` — the ASR sidecar

The Python half of kvit-dictate: the **reference server**. It receives 16 kHz mono s16le audio
over a WebSocket, runs one ASR backend, and sends `partial` / `final` messages back.

**`--listen` is the shipped binding; `--pipe` is the bench's.** release-plan.md §1 made the
recogniser something the user configures rather than a child process the client spawns, so this
program now listens on a port instead of connecting to a pipe its parent created. The pipe
binding stays for `bench/`, which genuinely does launch its subject locally. It has no window, no tray icon, no
settings, and no opinion about what happens to the text: plan.md §1.1 splits the product in two so
that each new model candidate is a ~50-line adapter here instead of weeks of C++ per runtime.

* The wire contract is `docs/protocol.md`. It is frozen; where this README and that document
  disagree, that document wins.
* The architecture argument is plan.md §1.1–§1.2. The environment policy is plan.md §5.
* The candidate models and which environment each needs are `bench/CANDIDATES.md` §3–§4.

```
server/
├─ pyproject.toml           package metadata; ZERO required runtime dependencies, by design
├─ requirements/            one file per backend environment (index in §6 below)
├─ kvit_asr/
│  ├─ __main__.py           `python -m kvit_asr` entry point
│  ├─ server.py             transport, handshake, model load, message routing
│  ├─ config.py             command line, model-directory resolution, offline policy
│  ├─ protocol.py           frame codec + message catalogue (shares fixtures with the C++ side)
│  ├─ ws.py                 the WebSocket binding (docs/protocol.md §2.6) — SHIPPED
│  ├─ transport.py          the named-pipe client, via ctypes against kernel32 — bench only
│  ├─ session.py            one session's decode loop: audio buffer, VAD, utterance/seq numbering
│  ├─ stability.py          `stable_bytes` (plan.md §1.4) — the same algorithm as the C++ engine
│  ├─ vad.py                Silero ONNX with a stdlib energy fallback; utterance endpointing only
│  └─ backends/             one module per model runtime; every heavy import is inside `_load()`
└─ tests/                   stdlib unittest (§7)
```

---

## 1. The core is stdlib-only, and that is load-bearing

`kvit_asr` imports nothing that is not in CPython 3.12's standard library. Transport, framing,
JSON, the session loop, the stability computation and the mock backend all run on a bare
interpreter with nothing installed. Three things depend on that and each is worth more than the
convenience of a dependency:

1. The `mock` environment costs nothing to create, so the entire C++ side — handshake, session
   lifetime, `seq` ordering, `stable_bytes` monotonicity, commit engine, injection, overlay — can
   be exercised end to end with no GPU and no model, and a failure is attributable to the C++ side
   rather than to a model.
2. A backend whose runtime is missing disables **that backend**, not the server. Every third-party
   import lives inside a backend module and happens at load time, so the server can still answer
   `hello` and report `model_load_failed` with a message a human can act on.
3. plan.md §5.1 keeps `..\wan-dancer\.venv` read-only, and the Whisper baseline runs *inside* that
   venv under an interpreter this package must not add anything to. That is only possible because
   importing `kvit_asr` pulls in nothing.

Named-pipe I/O is therefore `ctypes` against `kernel32`, not `pywin32`, and the tests are stdlib
`unittest`, not pytest — a test runner that had to be installed first would make claim 1 untestable
in the one environment where it matters most.

---

## 2. How it is launched

**By `kvit-dictate.exe`, not by a human.** The parent does all of this before the sidecar exists
(docs/protocol.md §2.4, plan.md §1.2):

1. generates a **per-launch unpredictable pipe name** — `\\.\pipe\kvit-dictate-<32 hex>` — which is
   what defeats pipe squatting by a process that started first;
2. creates the pipe with an explicit DACL built on the **logon SID** (the default descriptor grants
   read to Everyone and to the anonymous account — plan.md §1.2 corrects revision 1 on this);
3. spawns the interpreter that `models.json` names for the selected model, passing that pipe name on
   the command line;
4. validates the connected peer with `GetNamedPipeClientProcessId` before speaking protocol.

Exactly one transport is chosen per process, and giving neither or both is a usage error.

With `--listen` this is a server: it binds, accepts WebSocket clients, and gives each one its own
handshake, its own `ready` and its own session (docs/protocol.md §2.7). With `--pipe` it is the
pipe *client*, connecting to a name its parent gave it. `--port` and `--host` are still in the
parser, but they now point at `--listen` rather than refusing the idea of a port — the decision
they used to cite is the one release-plan.md §1 reversed, and an error message that cites a
superseded rule sends the reader somewhere useless.

```
<interpreter> -m kvit_asr
    --listen      [host:]port                                 (the shipped binding)
    --max-connections <int>                                   (0 = unlimited; default 1)
    --pipe        \\.\pipe\kvit-dictate-<32 hex>   (the bench binding; from the parent)
    --backend     mock | sherpa-onnx | transformers-whisper | faster-whisper
    --model       <id or path as spelled in models.json>
    --model-dir   <directory of unpacked model files>        (optional)
    --chunk-ms    <int>                                       (optional; backend-specific)
    --decoding    greedy_search | modified_beam_search        (optional; backend-specific)
    --device      auto | cpu | cuda | dml
    --backend-opt KEY=VALUE                                   (repeatable)
    --parent-pid  <pid>                                       (optional peer check)
    --log-level   debug | info | warning | error
    --log-transcripts                                         (OFF by default — see §5)
```

**One backend per process.** `--backend` is read once and never changes. Selecting a different model
means the parent starts a **new** process under that model's interpreter, because the
model→interpreter mapping lives in the C++-side `models.json` and no single interpreter can import
every candidate's runtime (plan.md §1.1). Do not turn this into a runtime registry; `server.py`'s
header explains at length why that "improvement" is wrong rather than merely unnecessary.

**Finding `kvit_asr`.** The package is *not* installed into any of the environments below — the
`mock` environment has nothing installed at all, which is the point of it. The launcher must
therefore put `D:\projects\kvit-dictate\server` on the child's `PYTHONPATH` (or set its working
directory there). An editable install would also work for the environments that have packages, but
using it only for some of them would mean two launch mechanisms for no gain.

**Exit codes.** The parent reaps this process and reports why it died, so these are part of the
contract with the launcher even though they are not part of the wire protocol (`server.py`):

| Code | Meaning |
|---|---|
| 0 | normal exit |
| 2 | usage error (argparse) |
| 3 | could not connect, or the pipe broke — the parent is gone or never came |
| 4 | protocol version rejected (docs/protocol.md §5); the peer was told before exiting |
| 5 | model load failed |
| 6 | fatal protocol error (§11.3) |
| 7 | internal error |
| 8 | `--parent-pid` did not match the pipe's server process |

---

## 3. Running it by hand

### 3.1 As a server (what you actually want)

One command, and nothing has to create anything first:

```
py -3.12 -m kvit_asr --listen 127.0.0.1:10402 --backend mock
```

Point the client's **Settings → Server** at `ws://127.0.0.1:10402/` and press Test, or check it
from a shell with `kd_probe --server ws://127.0.0.1:10402/`. `docs/setup.md` covers the real
backends, and §5 of it covers why you should bind loopback rather than `:10402`.

`--max-connections 1` is the default and a second client is refused with `server_busy` rather
than quietly given a second copy of the model. Raise it if your backend can share.

### 3.2 Against the pipe binding, for debugging the bench path

The sidecar is the pipe client there, so something must create the server end first. Normally
that is `kvit-dictate.exe`. When the thing being debugged *is* the sidecar, this PowerShell
harness creates a pipe, launches the child against it, and does the `hello` exchange — no C++
build required. Use `--backend mock`, which needs no model and no GPU.

```powershell
$name = "kvit-dictate-debug-$([guid]::NewGuid().ToString('N'))"
$pipe = New-Object System.IO.Pipes.NamedPipeServerStream(
    $name, [System.IO.Pipes.PipeDirection]::InOut, 1,
    [System.IO.Pipes.PipeTransmissionMode]::Message,
    [System.IO.Pipes.PipeOptions]::None, 65540, 65540)

$env:PYTHONPATH = "D:\projects\kvit-dictate\server"
$child = Start-Process -PassThru -NoNewWindow -FilePath "C:\Python312\python.exe" `
    -ArgumentList "-m","kvit_asr","--pipe","\\.\pipe\$name","--backend","mock","--log-level","debug"

$pipe.WaitForConnection()
function Send-Ctl([string]$json) {          # 4-byte LE length COVERING the type byte, then 0x01
    $p = [Text.Encoding]::UTF8.GetBytes($json)
    $f = [byte[]]::new(5 + $p.Length)
    [BitConverter]::GetBytes([uint32]($p.Length + 1)).CopyTo($f, 0)
    $f[4] = 1
    $p.CopyTo($f, 5)
    $pipe.Write($f, 0, $f.Length); $pipe.Flush()
}
function Read-Ctl() {
    $h = [byte[]]::new(4); if ($pipe.Read($h, 0, 4) -lt 4) { return $null }
    $len = [BitConverter]::ToUInt32($h, 0)
    $b = [byte[]]::new($len); $got = 0
    while ($got -lt $len) { $got += $pipe.Read($b, $got, $len - $got) }
    return [Text.Encoding]::UTF8.GetString($b, 1, $len - 1)   # skip the type byte
}
Send-Ctl '{"t":"hello","proto":1}'
1..5 | ForEach-Object { $m = Read-Ctl; if ($m) { Write-Output "<- $m" } }
$pipe.Dispose(); if (-not $child.HasExited) { $child.Kill() }
```

Notes, because each of these has already caused a confusing minute:

* **This harness is not the real thing.** It creates a pipe with .NET's *default* security
  descriptor, which is exactly what plan.md §1.2 says must not ship. It is a debugging aid on a
  developer's own desktop, never a template for the C++ launcher.
* Diagnostics are one JSON object per line on **stderr**. `stdout` is not the transport
  (docs/protocol.md §2.1) and never was — but a C extension can still print to it, which is
  precisely why the transport is a pipe and not stdio.
* The sidecar exits promptly when the pipe breaks (`ERROR_BROKEN_PIPE` means the parent is gone).
  Disposing the pipe is enough to stop it; the `Kill()` is belt and braces.
* Exercised on 2026-08-16 far enough to prove that the child connects and that a control frame
  crosses in each direction. It is a hand-driven harness, not a test — the tested path is
  `tests/` (§7).

---

## 4. One environment per backend, and why disk is the cheap side of that trade

**The rule: every runtime gets its own locked environment, and nothing is ever installed into
`..\wan-dancer\.venv`.** Not `faster-whisper`, not NeMo, not `qwen-asr`, not an "obviously
compatible" pin, not under a `--dry-run` justification.

The reason is plan.md §5.1, which reverses revision 1 on exactly this point. `D:\projects\wan-dancer\.venv`
is **another project's working GPU stack** — python 3.12.9, torch 2.10.0+cu130 with `sm_120`
compiled in, transformers 5.14.1, and `sageattention 2.2.0+cu130torch2.10.0andhigher.post6`
compiled against that exact torch build. A clean dependency resolution is not evidence that an
added package leaves an untested import or runtime path intact, and guarding three package names is
too narrow a check to be worth anything. **Corrupting a working stack is a far worse and far less
reversible cost than spending disk**: the venv is not reproducible on demand, and a spare 4 GB is.

So we *run* its existing Whisper baseline (row D of `bench/CANDIDATES.md`) and install nothing. If
that venv ever stops working for us, the response is a new locked environment under `envs/`, created
by a human — never a package added to that one.

The count is kept down by **not needing torch**, rather than by sharing one environment
(plan.md §5.2). `bench/CANDIDATES.md` §3 settled how far that goes, and the answer was better than
hoped: the entire streaming half of the candidate set runs on one `onnxruntime` + `sherpa-onnx`
environment with no PyTorch at all, and **NeMo need never be installed**.

| Environment | Backends | Torch? | Approx. size | Status |
|---|---|---|---|---|
| `envs/mock` | `mock` | no | 0 | create first; it is `models.json`'s default |
| `envs/onnx` | `sherpa-onnx` (Nemotron ×2, zipformer) | no | ~60 MB | the Phase 1 workhorse; **CPU EP is the baseline plan** |
| `envs/onnx-gpu` | same, CUDA execution provider | no | ~350 MB | the experiment — see `requirements/onnx-gpu.txt` |
| `envs/ct2` | `faster-whisper` (rows E, E′) | no | ~150 MB (+CUDA DLLs) | second tier; not needed until picks 1–4 have run |
| *(none — existing)* | `transformers-whisper` (row D) | yes, already there | 0 | `..\wan-dancer\.venv`, **read-only** |
| ~~`envs/nemo`~~ | — | — | multi-GB | **do not create.** CANDIDATES.md §5.2: the ONNX exports make it unnecessary |
| ~~`envs/qwen-asr`~~ | — | — | multi-GB | no adapter exists; Qwen3-ASR is a batch reference only (CANDIDATES.md §5.3) |

`envs/` is gitignored (plan.md §2). The authoritative, reproducible definitions live in
`server/requirements/` **in this repository**, so kvit-dictate is rebuildable on its own;
`..\wan-dancer\asr-models.md` is a useful cross-reference ledger, not a dependency.

---

## 5. Two things to set before creating anything

### `UV_CACHE_DIR` must be on `D:`

```bat
set UV_CACHE_DIR=D:\uv-cache
```

uv **hardlinks** packages out of its cache into each venv rather than copying them. Hardlinks cannot
cross volumes. So a cache on `C:` with venvs on `D:` silently falls back to a full copy of every
package in every environment — no error, no warning, just several gigabytes and a slow install that
looks normal (plan.md §5.2 item 4). Set it in the shell you create environments in, or set it once
for the user; either is fine as long as it is set *before* the first `uv venv`.

`uv` is at `C:\Users\sk\.local\bin\uv.exe`.

### `HF_HOME` is shared, already set, and already populated

```
HF_HOME=c:\apps\llama-swap\gguf\hf
```

Set globally on this machine, so model weights are shared regardless of how many environments exist
and nothing is downloaded twice (plan.md §5.2). Already cached, confirmed on disk:

| Weights | Size | Used by |
|---|---|---|
| `openai/whisper-large-v3-turbo` | 1.5 GB | row D, the baseline — nothing to download |
| `Systran/faster-whisper-base.en` | 141 MB | a smoke test for `envs/ct2`; **not** a candidate |
| `ggerganov/whisper.cpp` ggml | 42 MB | not used by any current backend |

`models.json` repeats `HF_HOME` in the child's environment for the baseline entry, so the sidecar
does not depend on the parent's environment happening to carry it.

**The sidecar downloads nothing, ever.** plan.md §1.9: no network access during dictation, and model
fetching is an explicit, separate, user-initiated action. `config.apply_offline_policy()` sets the
libraries' offline flags before any backend is imported, and a missing model produces an error
carrying the `hf download …` command rather than a multi-gigabyte surprise mid-session. Recognised
text is never logged unless `--log-transcripts` is passed, which is off by default and should stay
off outside a specific debugging session.

---

## 6. Creating each environment

Run these from `D:\projects\kvit-dictate`. Nothing below has been executed by any agent — plan.md's
"install nothing" scope holds until a human decides otherwise, which is also why every pin in
`requirements/` is marked provisional and should be regenerated with `uv pip compile` on first
install.

### `envs/mock` — do this one first

```bat
set UV_CACHE_DIR=D:\uv-cache
uv venv --python 3.12 envs\mock
```

That is the whole thing. **Do not install anything into it** — an empty environment is what proves
the zero-dependency claim in §1, and the test suite must pass inside it.

### `envs/onnx` — the Phase 1 workhorse (CPU execution provider)

```bat
set UV_CACHE_DIR=D:\uv-cache
uv venv --python 3.12 envs\onnx
uv pip install --python envs\onnx\Scripts\python.exe -r server\requirements\onnx.txt
```

### `envs/onnx-gpu` — the experiment, not the baseline

```bat
set UV_CACHE_DIR=D:\uv-cache
uv venv --python 3.12 envs\onnx-gpu
uv pip install --python envs\onnx-gpu\Scripts\python.exe -r server\requirements\onnx-gpu.txt
envs\onnx-gpu\Scripts\python.exe -c "import onnxruntime as o; print(o.__version__, o.get_available_providers())"
```

That last line is not optional politeness. `bench/CANDIDATES.md` §6 item 1 records that whether the
official `onnxruntime-gpu` wheel carries `sm_120` (Blackwell) kernels for Windows **could not be
verified** — the upstream issue asking exactly that was closed stale with no maintainer answer. A
silent CPU fallback inside a GPU-labelled environment would show up only as a latency number nobody
could explain. Never install `onnxruntime` and `onnxruntime-gpu` into the same environment; they
provide the same module and which one wins is an installation-order accident.

### `envs/ct2` — faster-whisper, second tier

```bat
set UV_CACHE_DIR=D:\uv-cache
uv venv --python 3.12 envs\ct2
uv pip install --python envs\ct2\Scripts\python.exe -r server\requirements\ct2.txt
```

Needs CUDA 12 + cuDNN 9 DLLs on `PATH` for a GPU run and, unlike the baseline's environment, has no
torch to supply them; this machine's system CUDA (12.9 / 12.1) may already be enough, and
`requirements/ct2.txt` carries commented `nvidia-*` wheels for when it is not. `bench/CANDIDATES.md`
§6 item 2 records third-party, **unverified** reports of `CUBLAS_STATUS_NOT_SUPPORTED` on RTX
50-series with int8 compute, worked around by forcing `compute_type=float16`; the backend defaults
to `float16` on CUDA for that reason and says so in its log line. Smoke-test it with the model that
is already cached before downloading 3.1 GB:

```bat
envs\ct2\Scripts\python.exe -m kvit_asr --pipe \\.\pipe\... --backend faster-whisper ^
    --model Systran/faster-whisper-base.en
```

### The Whisper baseline — nothing to create

```bat
D:\projects\wan-dancer\.venv\Scripts\python.exe -m kvit_asr --pipe \\.\pipe\... ^
    --backend transformers-whisper --model openai/whisper-large-v3-turbo
```

with `PYTHONPATH=D:\projects\kvit-dictate\server`. Read §4 again before typing `uv pip install` with
that interpreter for any reason.

### Model weights, when a backend needs one that is not cached

```bat
set HF_HOME=c:\apps\llama-swap\gguf\hf
hf download Systran/faster-whisper-large-v3
```

(older Hugging Face CLIs spell this `huggingface-cli download`). The sherpa-onnx models are
per-chunk-size repositories — `bench/CANDIDATES.md` §2 rows A′/B′ list the exact names — and are
unpacked under `%KVIT_ASR_MODELS_DIR%`, which the ONNX and CTranslate2 backends resolve against.
Every backend's "weights missing" error prints the exact command for its own model.

**Known inconsistency, recorded rather than silently patched:** two environment variables name the
same idea. `config.py` falls back to `%KVIT_MODEL_DIR%` for `--model-dir`, while the sherpa-onnx and
faster-whisper backends read `%KVIT_ASR_MODELS_DIR%` for their own model roots. Either set both or,
better, always pass `--model-dir` explicitly from `models.json`, which bypasses the question. It
should be collapsed into one spelling; it is called out here so the next person to hit it knows it
is a wart and not a subtlety.

### `requirements/` index

| File | Environment | What it installs | Approx. |
|---|---|---|---|
| `mock.txt` | `envs/mock` | nothing, deliberately | 0 |
| `onnx.txt` | `envs/onnx` | onnxruntime, sherpa-onnx, numpy, soundfile | ~60 MB |
| `onnx-gpu.txt` | `envs/onnx-gpu` | as above with `onnxruntime-gpu` | ~350 MB |
| `ct2.txt` | `envs/ct2` | faster-whisper, ctranslate2, numpy, soundfile | ~150 MB |
| `whisper-transformers.txt` | *(none)* | nothing — documents the read-only venv | 0 |

There is no `nemo.txt` and no `qwen-asr.txt`, and their absence is a decision rather than an
oversight: `bench/CANDIDATES.md` §5.2 concluded NeMo need never be installed, and §5.3 concluded
Qwen3-ASR is a batch accuracy reference rather than a streaming candidate. A pinned file for an
environment nobody should create reads as an invitation to create it.

Silero VAD (`kvit_asr/vad.py`, plan.md §3 Phase 2 item 3) needs `onnxruntime`, which `envs/onnx`,
`envs/onnx-gpu` and `envs/ct2` all already have — hence no separate environment. It also needs the
`silero_vad.onnx` file itself, which is configuration, not a download the sidecar performs; without
it the module falls back to a stdlib energy detector that is honestly worse and says so.

---

## 7. Tests

```bat
C:\Python312\python.exe -m unittest discover -s server\tests -t server
```

`-t server` makes `server\` the import root so `import kvit_asr` resolves without installing
anything. The suite must also pass under `envs\mock\Scripts\python.exe`, which is the environment
with nothing in it — that run is what keeps §1 honest.

Some suites read `tests/fixtures/protocol/index.json`, which the **C++ suite consumes as well**.
That is the point of them: a divergence between the two endpoints' framing, UTF-8 offset or
grapheme-boundary behaviour shows up as a failing fixture on both sides rather than as garbled text
in someone's document.

---

## 8. Adding a backend

1. Write `kvit_asr/backends/<name>.py`. Subclass `Backend`, or `PseudoStreamingBackend` if the model
   is a batch model driven by a window *we* built. Every heavy import goes inside `_load()`.
2. Declare `BackendCapabilities` honestly — especially `mode` and `supports_context`. plan.md §3
   requires the streaming mode to be recorded because a batch model wrapped in a sliding window is
   not latency-comparable with a natively streaming one, and plan.md §4 scores context-enabled and
   context-free runs in separate columns. A backend that accepts `start.context` and quietly
   discards it puts a context-free run in the context-enabled column, which is worse than declaring
   `false`.
3. Register it in `backends/__init__.py` as `"name": ".module:ClassName"` — a lazy string, never an
   import.
4. Add `requirements/<env>.txt` with a comment naming its purpose and its approximate download size,
   and add the environment to §4's table and §6's commands.
5. Add a `models.json` entry naming the interpreter. The C++ side owns that file.
6. Mark every API detail you could not verify. This project's Phase 0A exists because plausible
   confidence about an unchecked artifact is the failure mode that costs the most later; the
   existing backend docstrings all carry an explicit VERIFIED / UNVERIFIED section, and
   `faster_whisper.py`'s says plainly that *nothing* in it was checked against an installed package,
   because no such package exists on this machine.
