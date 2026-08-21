# kvit-speech

A long-lived helper process that puts the **on-device Windows AI speech model** behind the
`windows-speech` backend in `server/kvit_asr/backends/windows_speech.py`.

It exists because `Microsoft.Windows.AI.Speech` is a WinRT API in the Windows App SDK and the
sidecar is stdlib-only CPython. Reaching WinRT from that interpreter would mean a projection and a
Windows App SDK bootstrapper inside it, ending the stdlib-only rule for *every* backend. So the
WinRT lives here, in its own process, and the sidecar talks to it over a small framed protocol.

Nothing else in kvit-dictate depends on this directory. If the Speech API is ever withdrawn, or
graduates and changes shape, this is the only thing that has to change.

---

## Build

```
dotnet build -c Release tools/kvit-speech
```

Needs the **.NET 9 SDK**. The output lands in
`bin/Release/net9.0-windows10.0.26100.0/win-x64/kvit-speech.exe`, which is where the backend looks
for it by default. `KVIT_SPEECH_EXE` overrides the search if you put a copy somewhere else.

The output tree is large (~200 MB) because `WindowsAppSDKSelfContained` carries the Windows App
SDK runtime next to the exe. That is deliberate — see *The API is experimental* below. It is
gitignored.

## Install the model

The speech model is an **optional Windows component**. On a Copilot+ PC it is preinstalled on the
NPU; on a CPU-only machine it is not, and Windows downloads it in the background through Windows
Update on first use. That download is not something a dictation session should start on its own
(plan.md §1.9), so the helper refuses and tells you how to opt in:

```
tools\kvit-speech\bin\Release\net9.0-windows10.0.26100.0\win-x64\kvit-speech.exe --ensure-model
```

It reports progress on stderr and exits when the model is `Ready`. Took under a minute on this
machine. You can remove the model again at **Settings → System → AI Components**.

## Check it works

```
tools\kvit-speech\...\kvit-speech.exe
```

with no arguments starts the helper and waits for frames on stdin, which is not useful by hand —
but stderr will tell you immediately whether the model was found:

```
[kvit-speech] native search path: C:\Program Files\WindowsApps\Microsoft.Windows.AI.Speech...\winml
[kvit-speech] GetReadyState() = Ready
```

To exercise it properly, use the backend: `server/tests/test_windows_speech.py` covers the process
seam with a fake helper, and the real one is exercised by selecting `windows-speech` in
`settings.json` and dictating.

---

## What had to be measured, because the documentation is wrong or silent

All measured 2026-08-21 on an i7-14700K (20 cores, 3.4 GHz, 33 MB L3), Windows 11 build 26200, no
NPU, so the CPU execution provider throughout.

### The API is experimental, not "1.7.1 or later"

Microsoft's documentation gives the prerequisite as "WinAppSDK version 1.7.1 or later". That is
not true. `Microsoft.Windows.AI.Speech` does not exist in any **stable** Windows App SDK: 2.4.0
restores `Microsoft.Windows.AI.Text`, `.Imaging`, `.Video`, `.ContentSafety` and
`.MachineLearning` projections and **no Speech projection at all**. It appears in the experimental
channel — verified in `2.2.2-experimental9` and `2.3.2-experimentala`.

The package reference is pinned to `2.3.2-experimentala`. Moving it to a stable version will
remove the API. Moving it forward within the experimental channel is a change that has to be
re-measured, because nothing here carries a stability guarantee — the compiler says so on every
build, as `CS8305`, and those warnings are left visible on purpose.

`WindowsAppSDKSelfContained` is on so that using this backend never requires installing a preview
runtime system-wide. Confirmed: the helper runs with only the stable runtimes registered on the
machine.

### Package identity is *not* required

The docs say a Windows AI API needs the app "packaged as an MSIX package with the
`systemAIModels` capability declared in `Package.appxmanifest`". Taken literally that would force
kvit-dictate's whole process tree to acquire package identity, since this helper is a grandchild
of `kvit-dictate.exe`.

What actually happens in an unpackaged process:

| Call | Result |
|---|---|
| `SpeechRecognitionModel.GetReadyState()` | works — returned `NotReady`, then `Ready` after the download |
| `SpeechRecognitionModel.EnsureReadyAsync()` | works — downloaded and installed the model |
| `SpeechRecognitionModel.TryCreateAsync()` | **fails**, `COMException 0x8007007E` |

`0x8007007E` is `ERROR_MOD_NOT_FOUND`. Not an access denial and not a policy refusal: a DLL that
could not be found. The speech implementation ships *inside* the component package
(`Microsoft.Windows.AI.Speech.Preview.CPU`, or its NPU sibling) in a `winml\` subfolder —
`asrapi.dll`, `asrmodelapi.dll`, `vadapi.dll`, `cacheapi.dll`. A packaged process resolves those
through its package graph; an unpackaged one has no package graph, so the loader never looks.

Putting that folder on the DLL search path is the entire fix, and `SpeechRuntime.cs` does it.
`LoadLibraryEx` on each of the four by full path succeeds from an unpackaged process, so no ACL
stands in the way either.

This is a documented-behaviour *gap*, not a documented feature, and it is the most likely thing
here to break on a future Windows update. The symptom would be exactly `0x8007007E` from
`TryCreateAsync`, and the fallback is to give this helper — and only this helper — a sparse
package identity.

### The model is quantized Whisper Small, and it hallucinates over silence

The component package contains `whisper_small_encoder.quant.onnxe` (107 MB),
`whisper_small_decoder.quant.onnxe` (181 MB), `tokenizer_small.json`, `vad.all.onnxe`,
`itn_asg.bin` (inverse text normalisation), `en_cap.bin` and `profanity.bin`. So "uses components
from OpenAI Whisper" means it *is* Whisper Small, int8.

Which means plan.md §1.6's warning applies in full. Two seconds of **digital silence** returned:

> `Rwy'n cael ei wneud i'r unrhyw ychydig.`

A fluent Welsh sentence, from an all-zero buffer. The server's VAD gate normally stops that window
ever being built; the backend's `silence_peak_threshold` is the second line of defence.

### Latency

```
SpeechRecognitionModel.TryCreateAsync()   2851 ms cold, 919 ms warm
```

so the helper must be long-lived. Decode, `BatchRecognition.Recognize(byte[])`, best of three:

| audio | 500 ms | 1 s | 2 s | 3 s | 5 s | 8 s | 15 s | 30 s |
|---|---|---|---|---|---|---|---|---|
| decode | 1434 | 1668 | 2112 | 2203 | 2984 | 3686 | 5715 | 10197 |
| ×RT | 2.87 | 1.67 | 1.06 | 0.73 | 0.60 | 0.46 | 0.38 | 0.34 |

≈ **1350 ms fixed + 0.30× real time**. The fixed floor is why `finalize_only` is the default: a
one-second hop needs a 1.7 s decode every second and can only fall further behind.

Reusing a `BatchRecognition` instance across calls saves nothing (2207 ms then 2269 ms for the
same 3 s window), so one is created per decode.

### Raw PCM is accepted directly

`BatchRecognition.Recognize(byte[])` takes the audio as bytes — no WAV container and no temp file.
Verified byte-identical transcripts for the same audio as a file path, as WAV bytes, and as raw
16 kHz mono s16le PCM. That is already the format docs/protocol.md §6.3 fixes, so nothing is
converted anywhere on this path.

### Streaming was measured and rejected for this seam

`StreamingRecognition` + `SpeechAudioProvider` works — the caller can push its own PCM rather than
letting the API open the microphone, which is what kvit-dictate needs. But its output shape does
not fit:

```
off=0     dur=4000   "The quick brown fox jumps over the lazy dog. This"
off=3960  dur=4000   " is a test of the Windows on-device speech recognition model."
off=7960  dur=1120   " of the Windows on Device Speech Recognition Model."
```

`Recognized` emits **overlapping four-second windows** whose text repeats across the seam, recased.
`Recognizing` never fired at all and `IsFinal` was never true. docs/protocol.md §8.1 requires
cumulative text, so using this would mean a de-overlap heuristic whose failures look exactly like
the stability violations §8.3 exists to reject.

Two more facts about it, recorded so nobody has to rediscover them:

* `StartContinuousRecognitionAsync()` must **not** be awaited before pushing audio. It does not
  complete on start; it runs for the life of the stream. Awaiting it first deadlocks.
* `provider.Dispose()` is the end-of-stream signal, and the thing that completes that task.
  `StopContinuousRecognition()` does not.

### The implementation DLLs print to stdout

Loading the quantized model writes a run of `[loader] INT8 zp size mismatch: got 0 expected 768`
lines to **stdout** — the same stream a stdio protocol would use. This is the hazard
docs/protocol.md §2.1 cites when it refuses stdio for the main protocol, except here it is not
hypothetical: it happens on every startup. `Program.cs` captures the real stdout handle and points
`STD_OUTPUT_HANDLE` at `NUL` before any speech DLL is loaded. Diagnostics go to stderr, which the
sidecar captures and includes in its error messages.

---

## The protocol

Normative copy is the header comment in `Program.cs`. In short — little-endian, length-prefixed,
same shape both ways:

```
[u32 length][u8 tag][payload...]        length counts the tag byte and the payload
```

| Direction | Tag | Payload |
|---|---|---|
| → helper | `T` | raw PCM, 16 kHz mono s16le — transcribe it |
| → helper | `P` | empty — ping |
| → helper | `Q` | empty — quit |
| ← helper | `R` | UTF-8 JSON, sent unsolicited exactly once when the model is ready |
| ← helper | `O` | UTF-8 text; empty is a valid answer |
| ← helper | `E` | UTF-8 message; the link stays usable |

Deliberately **not** the protocol in `docs/protocol.md`. That one is frozen, is between the C++
client and the Python sidecar, and carries sessions, sequence numbers and stability. This link
carries one question — "what do these bytes say?" — and must not grow a second implementation of
any of that.
