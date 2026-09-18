# progress — kvit-dictate

Written 2026-08-21; §3e and the 2026-09-09 entries in §6 and §8 added then, and the 2026-09-17
entry in §3e with it. Commit `41ef07e` on `main`.

Read with `plan.md` (the specification), `bench/CANDIDATES.md` (model picks),
`docs/supported-targets.md` (measurements + open questions §9) and
`tools/kvit-speech/README.md` (the local model's measurement record). This file is only: what
works, what does not, and what to do next.

---

## 1. Where we are

**Dictation works end to end against a real ASR model.** Press `Win+Y` in Notepad3, speak, and
your words are transcribed by the Qwen3-ASR server on `strixhalo.lan` and typed into the document,
with a live preview at the caret that follows it as text commits.

That sentence became true today. Before it, every word the product had ever produced came from a
scripted mock.

The thing that made it cheap was the **HTTP backend**: `server/kvit_asr/backends/openai_http.py`
posts audio to an OpenAI-compatible `/v1/audio/transcriptions` endpoint and is **stdlib-only**
(`urllib`, `wave`). No environment to build, nothing to download, no GPU stack to reconcile — it
loads under the same bare CPython the mock uses. `envs/` still does not exist and no model has
been downloaded.

**The one bug that made it unreliable is fixed, and confirmed by a human.** A long sentence used
to abort partway and lose its tail. Diagnosed from the log and the two sources and fixed without a
repro, then verified in a real dictation on 2026-08-18 at 22:09 — 47 seconds into Notepad3, seven
utterances committed, no violation. The two sessions it used to fail in died after about five. §3
has the mechanism.

**Since then the far end has moved under it, twice over** (2026-08-21): strixhalo switched to
vLLM and got 4–5× faster, and grew a Silero VAD route. Both are now used — see §3b. Together they
retired the two standing quality questions, and **Silero will not be run locally** (§6).

**And there is now a second real model that needs no far end at all** (2026-08-21, later):
`windows-speech` runs Microsoft's on-device speech model — quantized Whisper Small, an optional
Windows component — entirely on this machine. It is the first configuration that keeps plan.md
§2's "no network access during dictation" in the letter and not only the spirit, so §6's standing
question *"what does the product do when the far end is unreachable?"* now has an answer to choose
rather than only a problem to state. It is slower than the LAN (≈3.0 s for a 5 s utterance against
strixhalo's ≈550 ms) and its API is experimental. §3d has the whole thing.

---

## 2. What works, verified by a human

* **The whole pipeline**: hotkey → mic → VAD → HTTP transcription → stability → injection →
  overlay. Notepad3, `console=0`, `SendInput`.
* **Inline overlay at the real caret.** Notepad3 exposes a system caret, so source A wins at 0.95
  and the overlay tracks the true insertion point — logged moving 2738 → 2815 → 2918 → 3021 across
  one sentence while the pixel-diff estimator's 0.66–0.90 was correctly rejected in favour of it.
* **The floating pill**, as the low-confidence fallback, bottom-centre of the target's monitor.
* **Refusals**: password fields refuse before injecting.
* **The post-submission pill** (§3e): it appears after a session, the buttons act, it dismisses on
  a keystroke or an outside click, and it shrinks to the width of its buttons instead of lying
  across the text that was just typed. Every one of those was found by the operator using it —
  three of them were defects in the first build of it.
* **Four targets characterised** (observations, not measurements — `docs/supported-targets.md`
  §5.10): Notepad3 clean; Win11 Notepad garbles paced `SendInput`; VS Code's terminal clean;
  Windows Terminal detected as a console, text lands, overlay degrades to the pill.

### Test suites

```
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
build-windows-msvc-debug\bin\Debug\kd_tests.exe        # 648 tests / 39116 checks
cd build-windows-msvc-debug && ctest -C Debug          # 27 suites, none disabled

python -m unittest discover -s server/tests -t server  # 419 tests   (from the repo root)
python -m unittest discover -s bench/tests  -t .       # 129 tests   (from the repo root)
```

Use `C:\Python312\python.exe`. The `python` on PATH in Git Bash is a conda 3.9 and will fail.
`kd_tests.asrd_session` flakes under load — `partials.size() > 1` in the live-preview test — and
a rerun clears it. Seen once on 2026-09-09; it is a race in the test, not the product.

The installed build is rebuilt and reinstalled with `packaging/windows/build-release.ps1`
(**pwsh 7 only** — `powershell.exe` mangles its UTF-8 and reports parse errors that are not
there), then `dist\kvit-dictate-1.0.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`. Stop
the running instance first: `CloseApplications=yes` cannot close it with message boxes suppressed,
and the installer exits **5** having done nothing.

Kill any running `kvit-dictate.exe` before rebuilding — it holds the link output *and* the
single-instance mutex, so a stale one produces `LNK1168` **and** a new instance that exits
immediately, which is two symptoms of one cause.

### Runtime configuration

`%LOCALAPPDATA%\kvit-dictate\settings.json` — currently `model_id: "qwen-asr-http"`,
`overlay.mode: "inline"`, `log_level: debug`. A backup of the pre-real-model settings is beside it
as `settings.json.before-real-model`. The log at
`%LOCALAPPDATA%\kvit-dictate\logs\kvit-dictate.log` is the first thing to read after any session.

**Two different things control "how often does text appear", and they live in different files.**

1. **How often audio is transcribed** — the utterance boundary. `settings.json`'s
   `session.endpoint_silence_ms` is only a *hint* (`session.cpp:752` says so; `close_utterance()`
   flips a bool and audio keeps flowing). The authoritative boundary is the sidecar's, so the knob
   is `--endpoint-silence-ms` in **`models.json`**, now **1500 ms** for both qwen entries and both
   windows-speech entries (was 800, which split a sentence at every breath). The client-side value
   is kept at 1500 only so the two do not read as disagreeing.
2. **How often text is typed** — `settings.json`'s `cadence.mode`, now **`end-of-session`**. See
   §3c. The two are independent: the sidecar still decodes about a sentence at a time and still
   uploads at most an 8 s window, whatever the client does with the results.

Model entries that matter:

| id | what it is |
|---|---|
| `qwen-asr-http` | LAN Qwen3-ASR, re-transcribes once a second → live preview that revises. Used to trip the stability abort; fixed in `89d012e` and confirmed working (§3). Commits once per utterance, at the endpoint. |
| `qwen-asr-http-finalize` | one request per utterance, no live preview. Cannot trip it. The reliable one. |
| `mock` | scripted; the only entry that needs nothing at all. Still the way to test the C++ side in isolation. |
| `windows-speech` | **local, no network.** Microsoft's on-device model (Whisper Small int8) via `tools/kvit-speech`. Finalize-only — one decode per utterance. Slower than the LAN; see §3d. |
| `windows-speech-preview` | the same, with the hop on so the overlay revises. Lags on purpose; it exists so the lag can be seen rather than argued about. |
| `whisper-large-v3-turbo` | zero-install baseline: the weights (1.6 GB) and the venv both already exist on this machine. Never run. |

---

## 3. The bug that mattered — diagnosed, fixed, and confirmed

**A long sentence aborted mid-way and lost its tail.** Reproduced twice, on 2026-08-18 at 15:53
and 17:56. Fixed in `89d012e`, confirmed by a human dictation at 22:09 the same day: 47 seconds
into Notepad3, seven utterances committed at their endpoints, no violation and no `session error:`
line. Both failing sessions had died after roughly five seconds.

### What it actually was

Diagnosed without a repro, by reading both sides and replaying the sequence offline. The chain:

1. `qwen-asr-http` re-transcribes the **whole utterance** every second. A growing-window batch
   model does not extend its previous hypothesis, it replaces it — so once it can see where the
   sentence ended it re-cases the opening word and inserts the punctuation it was unsure of, and
   `so I was thinking we` comes back as `So I was thinking, we`.
2. `StabilityTracker` had already published a mark covering those bytes. §8.2 binds a new
   hypothesis against the *previously published* mark, so `_evaluate`'s step 0 raised
   `STABLE_PREFIX_MUTATED` — correctly, and with no value of `stable_bytes` that would have made
   the message conforming.
3. `Session._fail_session` turned that into `error{code:decode_failed, fatal:false}` + `stopped`.
4. `session.cpp`'s non-fatal `error`-frame branch aborted the session — and set **no** `o.error`.

**The diverging offset is 0.** The *first word* was revised, so no non-zero mark was ever safe.

### Two things §3 used to say that were wrong

* **It was not the C++ §8.2 check.** That check (`session.cpp`, the `check_stability` call) never
  ran, and neither did the commit engine's. Both aborts came through the error-frame branch four
  paragraphs up. The tell was in the log all along: `31 byte reason` is the length of the fixed
  string `committed text was contradicted`, which only `main.cpp`'s `SurfaceWarning` handler
  passes — `perform_commit` passes a much longer one.
* **Reading the new `session error:` line would not have worked.** That branch populated no
  `o.error`, so the next occurrence would have logged exactly what the last two did. Fixed in
  `4805a1d`; the branch now carries the child's code, message and session_id out.

### The fix, and why the other candidate could not have worked

`SessionConfig.publish_stable_partials`, derived in `ConnectionSession._start` from the backend's
own declared `pseudo-streaming` mode. A backend in that class publishes **no mark before the
`final`**; the tracker is not fed during partials at all.

Tuning `stability_n/m/hold_ms` — the first fix this file proposed — **cannot** work, and the
offset is the proof. The three knobs delay the mark; delaying a mark that will be contradicted at
byte 0 changes when the abort happens, not whether it does.

**The live preview survives.** `partial` still carries the full revising text and the client
renders everything above the mark as pending (plan.md §1.5), so with the mark pinned at 0 that is
the whole hypothesis. The words still appear as they are spoken; they land in the document at the
endpoint instead of a few hundred ms earlier. This is *not* `qwen-asr-http-finalize`, which has no
live preview at all.

`CommitEngine::Result::salvage` (7 tests) is still correct and still not on this path.

---

## 3b. The far end: linear and fast, and it now answers VAD questions

**Retraction, kept because it cost a day's conclusions.** This section once reported a latency
cliff between 8 s and 12 s of audio and concluded `window_ms` could never be raised. That was an
artifact of the harness: the sweep used synthetic harmonic noise, which drives the model into a
repetition loop until it hits its token cap (~7 s to return the 9 characters `'Whoa! Who'`). The
data said so and went unread — the *slow* responses returned the *least* text. **Measure this
endpoint with real speech or not at all.**

**Measured 2026-08-21, real speech** (SAPI, sliced from one 57 s utterance), after the server moved
to vLLM:

| audio | 2026-08-21 (vLLM) | before | ×realtime |
|---|---|---|---|
| 8 s | **237 ms** | 825 ms | 34× |
| 16 s | **344 ms** | 1501 ms | 47× |
| 30 s | **541 ms** | 2587 ms | 55× |

Linear, no discontinuity. `/health` reports `{"backend": "vllm", "vad": true}`.

### What bounds the window

Throughput, not a ceiling. Each hop re-uploads and re-decodes the **whole** window, so
`window_ms/50 + vad` must stay well under `chunk_ms`. Falling behind overflows the sidecar's
bounded audio queue (3000 ms) into a **fatal `overload_audio`**, so this wants margin.

Now **`window=30000`, `hop=2000`** — **~36% duty** including the VAD call, against ~73% before,
with nearly **four times** the window. A 30 s window makes the frozen-prefix stitch effectively
unreachable in normal speech; `window_slide` fired three times in two sessions at 8 s. If it ever
returns, raise `window_ms` **and `chunk_ms` together**.

### The VAD route, and why it is a correctness fix

`POST /v1/audio/vad` (Silero via pysilero-vad) answers whether a clip contains speech, in 50–176 ms
for 0.5–30 s. `vad_gate=true` asks it **before** transcribing.

This is not tidiness. Handed non-speech, the model **invents** — re-confirmed live 2026-08-21:

| input | model returns |
|---|---|
| 500 ms digital silence | `"Yeah."` |
| 2000 ms digital silence | `"Okay."` |
| key-click transients | `"Hi."` |

Under end-of-session cadence (§3c) those are typed into the document along with everything real,
and the log holds two 5-byte commits — the exact length of `"Yeah."` — both of which were Esc'd.
The VAD route rejects all three cleanly (`speech: false`) and passes real speech.

* **In the backend, not `kvit_asr.vad`.** That module is a frame-wise *streaming endpointer*
  deciding when an utterance ends, 32 ms at a time; this is a *batch* question about a finished
  buffer. Different shapes — so the gate does not replace the local VAD, it adds a
  better-informed opinion at the one moment that matters.
* **Fails open**: missing route, timeout, bad body → transcribe anyway. Losing real speech is
  worse than an occasional invented word, and a VAD outage must not silence dictation. It latches
  after the first refusal so a 2 s hop cannot log a warning every 2 s.
* **This is the silero question, answered without a local install**, and §6 records the decision
  not to install one. The server runs the same model with the operator's own tuning.

---

## 3c. Deferred commit (`cadence.mode: "end-of-session"`)

Text accumulates for the whole session and is typed once, at the end, in a single injection.
Currently **on**. `Cadence::Immediate` is the old behaviour and is still the default in code.

* **It costs nothing in audio.** Utterance boundaries and injection cadence were already fully
  decoupled — the sidecar decodes a sentence at a time into an 8 s window regardless (§3b).
* **Implemented across two files on purpose**, and the header says so: the engine's cadence gate
  covers only the partial path (`cadence_allows`, one call site — `on_final` never asks), so the
  engine suppresses early flushes and `App::flush_deferred_commits` does the accumulating. The
  engine still emits its op at each `final`, because a caller that buffers needs something to
  buffer.
* **The old `cadence` knob was dead** for this backend before this: every op comes from
  `on_final`, which does not consult it. Worth knowing before trusting any other §1.4 knob.

What happens to the buffer when the session ends:

| ending | what happens |
|---|---|
| hotkey again, click, keystroke, silence | **typed into the document** — all go through `finalise_session` |
| **focus moved to another window** | **clipboard**, plus a tray warning. It cannot be typed: `validate_target` requires the target to still be the foreground window, and the injector never restores focus because `SendInput` types into whatever *is* focused. |
| Esc | **discarded**, deliberately |
| crash / fatal error | lost, logged |

**The visible tail is 8 lines** (`kMaxLines`, raised from 3) and there is **no scrollback** — a
session-long buffer sits behind that window unseen. Front-elision means it always shows the newest
lines, so it reads as scrolled to the bottom. The overlay is on the floating pill because 8 lines
at the caret would cover the document being dictated into; the pill also no longer resizes to its
contents, so it stops jittering on every revision.

---

## 3d. The local model — `windows-speech`, added 2026-08-21

**Dictation no longer requires another machine.** `windows-speech` runs Microsoft's on-device
speech model through `Microsoft.Windows.AI.Speech`. Switching between it and the LAN is what it is
for every other model: `model_id` in `settings.json`. No C++ changed — `models.json`'s
`interpreter` + `module` + `args` already carried everything this needed.

Verified through the real `Backend` interface against a 7.5 s synthesized utterance: load 2802 ms,
one decode at the endpoint in 3486 ms, transcript exact. **Not yet verified by a human dictating
into a document** — that is item 1 of §6.

`tools/kvit-speech/README.md` carries the full measurement record. The five things that decide how
it is used:

**1. It is Whisper Small, int8.** The component package contains
`whisper_small_encoder.quant.onnxe` (107 MB) and `whisper_small_decoder.quant.onnxe` (181 MB). So
"uses components from OpenAI Whisper" means it *is* Whisper, and every Whisper caveat in this file
applies to it — including §4's invented commits over silence. Measured here: two seconds of
**digital silence** returned `Rwy'n cael ei wneud i'r unrhyw ychydig.`, a fluent Welsh sentence
from an all-zero buffer. The backend carries its own `silence_peak_threshold` gate behind the
server's VAD, and `test_windows_speech.py` asserts on the *request counter* rather than the text,
because that assertion has to fail even when the invention is plausible.

**2. It is slower than the LAN, and the shape of the cost is what matters.** Decode is
≈ **1350 ms fixed + 0.30× real time** on this i7-14700K.

| audio | 1 s | 3 s | 5 s | 8 s | 30 s |
|---|---|---|---|---|---|
| decode | 1668 ms | 2203 ms | 2984 ms | 3686 ms | 10197 ms |

strixhalo does 5 s in ≈550 ms (§3b), so this is ~5× slower. The **fixed floor** is the part with
consequences: a one-second hop needs a 1.7 s decode every second, so a live preview cannot keep up
and falls further behind within a sentence. `finalize_only` is therefore the default, which is the
same shape as `qwen-asr-http-finalize` and invisible under end-of-session cadence (§3c).

**3. The API is experimental, and the documentation is wrong about that.** Microsoft's page gives
the prerequisite as "WinAppSDK 1.7.1 or later". In fact `Microsoft.Windows.AI.Speech` exists in no
stable Windows App SDK — 2.4.0 ships the Text, Imaging, Video, ContentSafety and MachineLearning
projections and no Speech projection at all. `tools/kvit-speech` pins `2.3.2-experimentala` and
carries the runtime beside the exe, so this does **not** require a preview runtime system-wide.
`CS8305` warnings on every build are the API saying so; they are left visible.

**4. Package identity is not required, despite the documentation saying it is.** The docs demand
an MSIX with the `systemAIModels` capability, which would have forced identity on the whole
process tree. Measured: `GetReadyState()` and `EnsureReadyAsync()` both work unpackaged, and only
`TryCreateAsync()` fails — with `0x8007007E`, `ERROR_MOD_NOT_FOUND`. The implementation DLLs ship
*inside* the component package's `winml\` folder and an unpackaged process has no package graph to
resolve them through. Adding that folder to the DLL search path is the entire fix. **This is the
most likely thing here to break on a future Windows update**; the symptom is exactly that HRESULT
and the fallback is a sparse package identity for the helper alone.

**5. The WinRT is in a separate process, and that is not incidental.** `tools/kvit-speech` is a C#
helper the sidecar spawns and talks to over a small framed protocol. It is there so the sidecar
package stays stdlib-only — a WinRT projection and a Windows App SDK bootstrapper inside
`C:\Python312` would end that rule for *every* backend, not just this one. It also means the
blast radius of an experimental API is one directory.

It hijacks stdout before loading any speech DLL, because **those DLLs print to stdout**
(`[loader] INT8 zp size mismatch: …` on every startup). That is precisely the hazard
docs/protocol.md §2.1 cites when it refuses stdio for the main protocol, except here it is
observed rather than hypothetical.

### The streaming API — promising, not built, and this section used to be wrong

**Retracted.** This section previously said streaming emitted *"overlapping four-second windows
whose text repeats across the seam, recased"* and treated that as disqualifying. It was written
from a 7.5 s run whose last segment covered 7960–9080 ms while the speech ended at 7555 ms — so
the apparent duplicate was a **silence hallucination**, the same failure as the Welsh sentence,
not a seam artifact. The offsets were in the output and were not checked against where the speech
ended. Batch is still what is implemented; what is not true is that it was the *measured better*
choice. §8 has the habit that would have caught it.

Re-measured on 25.4 s of speech, pushed at real time with no padding, run twice with
byte-identical results. Segments are deterministic and essentially contiguous, and **no phrase
repeats across a seam**. The one real artifact is the opposite of a repeat — a **truncated word
fragment**: segment 1 ends `"… lazy dog. D"` and segment 2 begins `" Dictation …"`, so naive
concatenation gives `"dog. D Dictation"`. That would corrupt cumulative text under §8.1, but it is
a narrow characterised defect, not a structural mismatch.

**The reason to build it is latency, and the gap is not small:**

| | 25.4 s of audio |
|---|---|
| batch, whole utterance | 8740 / 9486 ms |
| streaming, tail after the last audio | **1703 / 1575 ms** |

Streaming overlaps decode with speech, so only the tail is left when the speaker stops. It also
needs no window cap: batch decode grows with the window, so `window_ms` truncates a long utterance
and `PseudoStreamingBackend` reassembles it from a frozen prefix. For a *short* utterance the two
are close — batch's ~1350 ms floor against streaming's ~1.6 s tail — so the win is concentrated
exactly where batch is worst.

Building it means a `Backend` declaring `stateful-chunked` that holds the stream open across an
utterance, accumulates segments, and repairs the seam fragment. **No C++ would change for it
either**: `native-streaming` and `stateful-chunked` are already declared modes the client speaks,
and `feed() -> list[Hypothesis]` is already the streaming shape. See §6.

Three mechanical facts, recorded so nobody rediscovers them: `Recognizing` **never fires** (three
runs, both audio lengths) and `IsFinal` is **never true** even at the real end, so neither can
detect finality; `StartContinuousRecognitionAsync()` must **not** be awaited before pushing audio
(it runs for the life of the stream, and awaiting it first deadlocks); and `provider.Dispose()` —
not `StopContinuousRecognition()` — is the end-of-stream signal and what completes that task.

---

## 3e. Delivery is unobservable, so the user is the witness — 2026-09-09

**`inject()` returning `Ok` is not evidence the text arrived**, and the clipboard method is where
that gap is widest: `inject_via_clipboard` returns `Ok` once the paste chord has been *accepted by
the input system*, which an application that binds `Ctrl+V` elsewhere — or ignores it — produces
identically with an empty document. The comment at its return has always said so. Nothing acted on
it.

That mattered because `clear_recovery` deleted the on-disk copy at exactly that moment. The one
failure that leaves no trace was also the one where the last copy was thrown away.

What now exists:

* **`privacy.keep_last_submission`** (default on) keeps the delivered text in `last-dictation.txt`
  — one dictation, overwritten by the next, never appended to. The tray grows **"Copy the last
  dictation"** whenever there is one. That tray item is the point: `recover.txt` was always in the
  folder the tray already opens and was never *there*, because delivery deleted it. Under the
  streaming cadences `perform_commit` accumulates and `finish_dictation` saves, or their "last
  dictation" would be the last word.
* **This is a persisted transcript, so plan.md §1.9 now names it** as its second exception beside
  `crash_recovery`, rather than leaving the argument in a comment.
* **A post-submission pill.** After a session it stays up showing **"Text didn't arrive?"** over at
  most two buttons: **Copy**, and **Send another way**. Which are offered is decided from the
  method that actually ran and from `ClipboardGuardState` — after a clipboard injection our text is
  normally *gone* from the clipboard, so **Copy** is still needed; only when the restore was
  skipped is the retry offered alone.
* **One label for both retries, on purpose.** "Type it again" and "Copy and paste" named the
  mechanism, and which appeared depended on what had already run, so one question wore two labels.
  The code still picks whichever method is untried; the button stops narrating it.
* **Clicks arrive through the overlay's own `WM_LBUTTONDOWN`**, not the low-level hook the compose
  buffer uses — the hook's pill handling is gated on `composing()`, false by then. `WS_EX_NOACTIVATE`
  means the target keeps focus across the click, which is the only reason a retype can work at all.
* **`FilterOutcome::dismiss_review`** closes it on any real key-down or any click outside the pill.
  Four things deliberately do not close it: a click *inside* it (that is how the buttons are
  pressed), a key-*up* (the hotkey that produced it is still down when it opens), our own injected
  input (the first character of a retype would dismiss the pill that asked for it), and anything at
  all when no review is open.
* **A retry writes the per-app policy**, dated, noting which method was *reported* failing. It is
  not evidence the retry worked — that is as unobservable as the first attempt — and the note does
  not claim it is. A hand-written note on an existing entry is carried into the new one; it is the
  only record of why somebody chose that method.

**Verified by a human**: the pill appears, the buttons work, dismissal works, the width shrinks to
the buttons. **Not verified**: the clipboard guard below, and no `app_policies` entry has yet been
written by a retry.

### The clipboard guard restored nothing, ever

plan.md §1.7 asks that the clipboard be restored "only if we still own the state we set", and
`GetClipboardSequenceNumber` was the whole test. Measured on this machine: `clipboard changed under
us (seq X -> X+6)` on **every** clipboard injection, always the same six, inside the 60 ms between
the set and the restore. The guard fired every time, the snapshot was never once put back, and the
clipboard method quietly meant *"kvit-dictate keeps your clipboard"* — the precise outcome the
guard exists to prevent.

**What produces those six is unidentified.** Two candidates were probed and neither reproduced it:
a running Ditto did not move the counter at all in the 560 ms after a clipboard write, and forcing
synthesis of `CF_TEXT`/`CF_OEMTEXT`/`CF_LOCALE` moved it by zero. Both were plausible; both are
wrong. The fix does not rest on knowing the answer — the sequence number now decides only whether
to *look*, and what decides the answer is whether `CF_UNICODETEXT` still holds the string we
published. Compared by content, not by handle, because anything re-publishing our string hands back
a different `HGLOBAL` with the same characters. The two outcomes log differently, so the next real
paste says which happened.

### The guard was fixed, and that made a race visible — 2026-09-17

Fixing the guard above made the restore *real* for the first time, and what it uncovered was worse
than what it fixed: **the paste started delivering the user's previous clipboard contents instead
of the dictation.** Reported by the user, who had been working around it for a week — every one of
the seven "copy last dictation to the clipboard" presses in the logs is after 2026-09-09 17:31, and
there is not one in the twelve days before it. That timestamp is the first run of the build
carrying `ea8d22f`.

The restore was a stopwatch: 60 ms after the paste chord, put the user's clipboard back, whether
or not the target had read ours yet. VS Code is Electron; its `Ctrl+V` crosses into the renderer
process and comes back before anything touches the clipboard, and under load that round trip is
routinely slower than 60 ms. The comment above the sleep had already said what would happen —
*"restoring immediately would hand it the previous contents"* — and 60 ms was the same guess with
a smaller number. Before `ea8d22f` the guard skipped every restore, so the race existed and was
invisible: our text simply stayed on the clipboard.

**The restore now runs from a `WM_TIMER`, not a sleep**, and waits `inject.paste_settle_ms` — now
1200 ms by default and floored at 400, because 60 is the value the bug was made of rather than a
preference anyone expressed. Deferring it is what makes the longer wait free: nothing blocks the
UI thread, so the wait costs only that the user's own clipboard is displaced for about a second,
during which the ownership check still protects anything they copy. Measured end to end against
`tools/testtarget`: five of five pastes verbatim, `on_clipboard_ms` between 1203 and 1218.

**Delayed rendering was built first, and rejected by measurement.** Publishing `CF_UNICODETEXT`
with a null handle makes Windows ask for the bytes with `WM_RENDERFORMAT` at the moment a reader
wants them, which is the completion signal the clipboard API otherwise lacks — restore when the
*target* asks, not when a clock says so. Against a plain Win32 `EDIT` control it produced **no
paste at all**: the control logged `WM_PASTE` and inserted nothing. The diagnostics named the
reason — `renders=1 last_requester_pid=26180`, which is **Ditto**, the clipboard manager running
on this machine. Only the first render request reaches the owner, Ditto answers it every time, and
the target's own read lands in whatever state Ditto leaves behind. The cost is not a late restore,
it is a lost paste. Real bytes are served by Windows from a handle that is already there, whatever
our process is doing and whoever else is listening. **Do not reintroduce delayed rendering without
re-running that measurement.**

That also answers §6.4b, which had been looking for what bumps the sequence number by six: Ditto is
on this machine and is demonstrably in the middle of every clipboard write we make. It did not
reproduce under the earlier probe because that probe watched the counter after a write rather than
asking who was reading.

Smaller things that had to be handled rather than discovered:

* **A second injection adopts the first one's snapshot** instead of taking a new one, which would
  snapshot our own dictation and faithfully restore *that*. `main.cpp` injects twice in a row
  routinely — the deferred flush, then the suffix — so this is the ordinary path.
* **`set_clipboard_text` abandons a pending restore.** It is the recovery copy; a timer wiping it
  a second after the user asked for it is the whole failure it exists to prevent.
* **The post-submission pill reads `restore_pending`** — our text is on the clipboard at that
  instant and will not be shortly, so **Copy** is still offered.
* **`App::shutdown` runs the restore before the message loop stops**, and `tools/probe` does the
  same at exit. A `WM_TIMER` that never fires would mean keeping the user's clipboard.
* **A restore that cannot open the clipboard is retried** — five attempts, 60 ms apart — rather
  than abandoned. The deferred design has somewhere to put a second attempt; the blocking one did
  not. A busy clipboard produced exactly this failure once while the tests were being written.
* **`paste_settle_ms` used to be clamped to 500 ms in silence** although `settings.cpp` accepts
  0..5000. Nothing is clamped now, and raising a value below the floor is logged.

**Verified**: the end-to-end measurement above, and the suite. **Not verified by a human in the
product yet**: a real dictation into VS Code, which is the report this came from.

### And injection is timed now

`InjectResult::elapsed_us` was filled in on every path and read by nobody (§8). It is logged at the
deferred flush with the resulting rate. This answered a real question — *"why does applying start
slow and speed up?"* — with the honest answer that it does not, on our side: pacing is a fixed 32
chars per `SendInput` and a fixed 1000 µs gap, bounded at about 60 ms for a 2000-character flush,
so anything the user watches after that is the receiving window draining its input queue. Measured
since: 335–700 chars/s into real targets under `sendinput`, and 1800–4550 chars/s under `clipboard`.

---

## 4. Also true, and smaller

* **The local endpointer stays as it is, and that is now a decision rather than a default.**
  Silero locally was the standing open item; §4b measures it and §6 closes it for good — the
  energy scorer is over-inclusive, not lossy, so a local Silero buys boundary precision only. The
  unverified "triggers on keyboard noise" claim also stays retired: over-inclusive is exactly what
  that claim predicted, and §3b's gate is the answer to it either way.

* **The model hallucinates over silence**, measured: 500 ms of digital silence returns `"Yeah."`,
  2000 ms returns `"Okay."` No guard is implemented on purpose — the endpoint returns a bare
  string with no timestamps to filter on, and a phrase blocklist would silently delete words a
  user really said. VAD gating upstream is the real fix, which is what makes the VAD above matter.
  **Re-confirmed 2026-08-21** against the live endpoint, and it is worse than "silence": 500 ms of
  silence returns `"Yeah."`, 2000 ms returns `"Okay."`, and synthetic key-click transients return
  `"Hi."` — both 5-byte strings, and the log contains two 5-byte commits (2026-08-21 00:12–00:13),
  both of which the operator pressed Esc on. Consistent with hallucination; not proof, since a real
  5-byte word is equally possible. The transcript-log test above is what would settle it.
* **Clicking or typing ends a session.** By design (plan.md §1.8). The sequence is: focus the
  target, *then* `Win+Y`, then do not touch the mouse. The log now names the gesture.
* **`bench/CANDIDATES.md` §4 is contradicted by testing.** It names Nemotron and zipformer as the
  streaming finalists; the operator has measured both as much worse quality than Qwen. Not yet
  recorded in that file. It matters more than a note: if the quality winner is a *batch* model,
  then reconciling revisions is the permanent architecture, not a bring-up compromise waiting for
  a streaming model to make it moot.

---

## 4b. Which VAD does what

Six things could be called "the VAD". Three of them run.

| | where | what it decides | status |
|---|---|---|---|
| **energy/ZCR endpointer** | sidecar, `vad.py`, frame-wise on 32 ms frames | **when an utterance ends** (`--endpoint-silence-ms`, 1500 ms) and what reaches the decoder | **running** — this is "the local VAD" |
| **`/v1/audio/vad`** (Silero) | strixhalo, batch, per window | **whether a window is speech at all**, before it is transcribed (§3b) | **running** — added 2026-08-21 |
| **`silence_peak_threshold`** | `windows_speech.py`, per window | whether to spend a local decode at all | **running** — added 2026-08-21, and it is a *correctness* gate, not a saving (§3d) |
| Silero ONNX in-process | sidecar, `SileroOnnxScorer` | would replace the energy scorer above | **not happening** — decided, see below |
| the Windows model's own VAD | inside the speech component (`vad.all.onnxe`, `vadapi.dll`) | its internal segmentation | **runs, and is not ours** — no API exposes it, no setting reaches it, and it did not stop the Welsh sentence over silence (§3d) |
| Media Foundation Voice Capture DSP | `MFPKEY_WMAAECMA_FEATR_VAD`, `AEC_VAD_*` | would be a frame-wise speech/non-speech flag in the C++ client | **not needed** — see below |

**On the Voice Capture DSP VAD.** It is real, it is public, and it is the wrong tool here. Three
reasons, in order of weight. It is a *frame classifier*, so it answers "is this frame speech?" and
not "has the speaker stopped?" — the endpointing state machine in `vad.py` would still have to
exist, and would still be the thing with the bugs. It is a property of the Voice Capture DMO,
which means adopting it means routing capture through that DMO and inheriting its AEC and its
resampling, a change to `audio_capture.cpp` far larger than the thing it buys. And the measurement
in §4b already says the local scorer is *over*-inclusive rather than lossy, so a better frame
classifier is not what is missing. Recorded as considered and declined, not as unexamined.

They are not alternatives. The first is a *streaming endpointer* answering "has the speaker
stopped?" incrementally; the second is a *batch classifier* answering "is there speech in this
buffer?" about a finished window. Only the first can drive utterance boundaries; only the second is
good enough to be trusted about content.

### Do we need Silero locally too? No, and that is now a decision — measured 2026-08-21

It is the same model the server runs, so a local copy could only add one thing: *streaming*
endpointing, the remote route being batch. The question is therefore narrow — **is the energy
scorer losing speech?** Its false *positives* no longer reach the document (§3b's gate catches
them), but audio it refuses to pass never reaches the backend at all and nothing can recover it.

57.4 s of real speech through both:

| | result |
|---|---|
| server Silero | 48.93 s of speech, 12 segments |
| local energy scorer | **58.70 s passed to the decoder**, 8 utterances — **120%** |

**It is over-inclusive, not under-inclusive.** It passes more than the whole file (pre-roll padding
re-emits lead-in) and loses nothing. Identical with and without lead-in silence, so the
calibration-window worry in its docstring did not materialise on a hotkey-style opening either.

Two consequences:

* **A local Silero would buy nothing that matters, so it is not being installed** (§6). What it
  would improve is boundary *precision* — 8 utterances against Silero's 12 segments — which is
  punctuation granularity, and invisible under end-of-session cadence. Not worth putting
  `onnxruntime` on this machine.
* **It also explains the `"Yeah."` commits.** Passing ~everything means digital silence was
  reaching the decoder unimpeded, and the model invents on silence (§4). The local scorer was
  never filtering content; §3b's gate is what does it now.

Caveat, since this file has been burned once today: clean synthesised speech in a silent file. A
real microphone with a fan or a conversation in the room is the case not covered, and it is the
one the docstring's "detects sound, not speech" is really about.

**The client (C++) has no VAD at all**, which was not the design — see §8.

---

## 5. Phase 0B, and why it is parked

15 of 180 cells (`kd_probe.exe --status`). `docs/supported-targets.md` has the detail; the
headline is that the phase produced a **negative** result, which plan.md §3 explicitly calls a
success for the phase rather than a failure:

* Source C (pixel differencing) is **accurate on a clean append** — 0.07 line heights on both
  measured applications — and **wrong by ~10 line heights on a mid-line insertion**, because text
  to the right reflows.
* **Its confidence score cannot tell those two apart.** Twelve cells on the calibration target
  supported a separating threshold; the first two cells of a real application destroyed it
  (`A10/S02`, wrong by 10.27 line heights, scores 0.3956 — *above* two accurate cells).
  §9 question 7 has the full argument.
* Therefore plan.md §1.5's deterministic degradation gate is **written and tested but deliberately
  not called** (`kd::overlay_mode_for`, `tests/test_overlay.cpp`, 12 tests). `App::apply_overlay_mode`
  carries the reason at the call site. This is the one piece of "tested but uncalled" code in the
  tree and it is flagged in four places on purpose.
* A caret survey found **only Notepad3** exposes a system caret among ~15 running applications —
  not Chrome, Edge, VS Code, Explorer or Windows Terminal. So the targets that would benefit most
  from inline are exactly the ones where it is least reliable.

**Parked deliberately.** The remaining Phase 0B work needs either an operator annotating frames or
machine-state changes, and none of it is on the path to a working product. It is also where several
days went that could have gone into §1. Do not resume it without a reason connected to a user-visible
problem.

---

## 6. What to do next

**Decided, not open — Silero will not run locally.** The sidecar keeps the stdlib energy/ZCR
endpointer. `onnxruntime` is not going on this machine and `SileroOnnxScorer` stays unconfigured.
The reasoning is measured in §4b: the energy scorer is *over*-inclusive (it passed 120% of the
speech the server's Silero found, losing nothing), so the only thing a local copy would buy is
utterance-boundary precision — punctuation granularity, invisible under end-of-session cadence.
The job that actually mattered, keeping invented words out of the document, is done by
`/v1/audio/vad` on strixhalo, which is the same Silero with the operator's own tuning (§3b).
`%LOCALAPPDATA%\screenpipe\vad\silero_vad.onnx` exists if this is ever revisited; it would need
a reason connected to a user-visible problem, and a noisy room is the one case §4b does not cover.

Actually next:

1. **Use it for a while and watch three lines.** Everything below §3 changed today and none of it
   has had a real day's dictation: `window_slide` should have stopped (30 s window), 5-byte
   `"Yeah."` commits should have stopped (the VAD gate), and a session should now survive past two
   minutes of continuous talking (§8's timer fix).
1b. **Dictate into a document with `model_id: "windows-speech"`.** §3d is verified through the
   `Backend` interface against synthesized speech, which is not the same claim: a real microphone
   in a real room is exactly the case §4b says is not covered, and this model invents over
   non-speech more readily than Qwen does. Two specific things to watch — whether the ~3 s wait
   after the endpoint is tolerable in practice under end-of-session cadence, and whether anything
   Welsh, or otherwise unrelated to what was said, reaches the document.
1c. **Decide whether to build the streaming arm of `windows-speech`** (§3d). Measured: on 25.4 s of
   speech, batch takes 8.7–9.5 s from the endpoint while streaming leaves a 1.6–1.7 s tail, and
   streaming needs no `window_ms` cap at all. The win is concentrated on long utterances, which is
   exactly where batch is worst and where the frozen-prefix reassembly is doing the most guessing.
   The cost is one characterised defect to repair — a truncated word fragment at the first seam —
   plus a `Backend` that holds the stream open and declares `stateful-chunked`. **No C++ changes**;
   the client already speaks that mode. Do §6.1b first: if ~3 s after the endpoint turns out to be
   fine in real use, this is optimisation and can wait behind items 2–5.
2. **Record the Nemotron/zipformer quality finding in `bench/CANDIDATES.md` §4.** The picks table
   still recommends models the operator has measured as worse than Qwen. Now overdue on two
   counts: `publish_stable_partials=False` means commit-at-the-endpoint is the permanent latency
   profile if the quality winner is a batch model, and §3b's numbers (34–55× realtime) invalidate
   the latency arithmetic the table's argument rests on.
3. **Decide what the product does when the far end is unreachable** — half-answered, and the
   remaining half is now a product decision rather than an engineering one. `windows-speech` (§3d)
   is a complete local path, so the dependency is no longer forced: plan.md §2 can be honoured by
   choosing that entry. What does *not* exist is any automatic behaviour. Selecting a model is a
   `settings.json` edit and a restart, so a LAN outage mid-session still ends that session; nothing
   detects strixhalo being down and nothing falls back. Whether it should is the open question, and
   it is a real one — a silent fallback would swap a 550 ms model for a 3 s one without saying so,
   which is its own kind of wrong. Also still true: strixhalo serves *both* transcription and VAD,
   so the qwen entries have two dependencies on it, not one.
4. **Dictate into VS Code with the paste method and read the log** (§3e). The restore is measured
   working against `tools/testtarget` and the log now prints `deferred clipboard restore ran after
   N ms`, but the report this fix came from was a real dictation into Code, and only a real
   dictation into Code closes it. What to look for: the dictated text arriving (not the previous
   clipboard item), and one restore line per injection with `restored=1`. Still genuinely
   unexecuted: the `set_clipboard_text` rescue for a deferred buffer whose target has gone (§3c),
   which is deliberately the one that does *not* restore.
4b. **~~Find what bumps the clipboard sequence by six~~ — answered 2026-09-17.** Ditto. It reads
   every clipboard write this machine makes; the delayed-rendering experiment in §3e caught it in
   the act, by process id, as the first and only answerer of a render request. The earlier probe
   missed it because it watched the counter after a write instead of asking who was reading.
5. **`settings.cpp`'s tests exist now and this item was stale.** `tests/test_settings.cpp` covers
   the round trip field by field, the fixed point, the defaults and the affix rules. What is still
   untested is the *dialog*: every control is read and written by hand in a 1900-line file, and two
   defects found on 2026-09-09 by looking at it — a checkbox wired to nothing, and a list that
   word-wrapped one rule into three — were both invisible to the suite.

---

## 7. Conventions

* **Commit after each working step, directly to `main`.** Verify before committing.
* No third-party C++ dependencies. No Qt. Everything in-repo, including the JSON parser.
* No header under `src/` includes `<windows.h>`; handles cross module boundaries as `uintptr_t`.
* The sidecar core is stdlib-only. Only individual backends may import numpy/onnxruntime/torch,
  each inside its own module, so a missing runtime disables one backend rather than the server.
* `..\wan-dancer\.venv` is **read-only**. Run its Whisper baseline; install nothing into it.
* Comments explain *why*. Where something is unverified, the code says so rather than asserting.
* **Measurements and observations are different things**, and `docs/supported-targets.md` keeps
  them in different sections. Nothing not taken with the probe may fill in a cell.
* **The document invariant is not negotiable** (plan.md §1.4): committed text is never rewritten,
  never taken back, no backspaces. Everything may append. When a backend misbehaves the client
  reports it and preserves the document — it does not repair it.

---

## 8. The recurring failure mode, six sessions running

**A contract implemented on one side and never called from the other.** The clearest instance yet
is §3's, because the failure mode struck *twice over* on the same incident and the second strike
was the fix for the first:

* `session.cpp`'s non-fatal `error` branch discarded the child's whole diagnosis. The sidecar had
  built it, sent it over the wire, and the receiver dropped it at one line.
* So the previous session added a reader for `o.error` — and added it to a path this abort does
  not take. The fix for "a diagnosis nobody reads" was itself never reached. **That is what made
  §6 item 1 wrong**: it prescribed one dictation to produce a log line that could not appear.

The habit that would have caught it: the log said `31 byte reason`, and 31 is the length of the
fixed string on one specific branch. The evidence identifying the true path was already in the
file that was being read for evidence.

**2026-08-21, and the costliest yet because it was user-visible.** `LocalEvent::SpeechDetected` is
the only writer of `last_voice_ms`, and it was raised from **nowhere** in `src/` — all four call
sites were in `tests/`. Both of plan.md §1.6's silence timers hang off that value, so:

* the endpoint timer, also guarded by `utterance_open` (set only in that same unreachable handler),
  **never fired at all**;
* the session timer has no such guard, so it measured **wall-clock since activation** rather than
  silence — a hard **two-minute cap on every dictation**, however much the user was saying.

A green suite said otherwise the whole time, because the tests raised the event themselves. That is
the shape to watch for: a test that supplies the input the product never supplies is testing a
function the product does not compute. Fixed by deriving the event from the sidecar's own
`partial`/`final` text — the component that owns the VAD — rather than adding a second detector.

**2026-09-09, three at once, and the third is a new variant.** A session spent on the injection
path turned up two more of the plain kind and one that a green suite could not have caught:

* **`InjectResult::elapsed_us`** was computed on every path through `injector.cpp`, and the header
  said it "feeds the UI/commit-delay term of plan.md §3 Phase 2 item 4". `grep` found no reader
  outside the file that wrote it. So when the operator asked why applying a long dictation starts
  slow, the measurement that answers it had been taken thousands of times and discarded every time.
* **`paste_ctrl_shift_v_default`** was serialised, parsed, drawn as a checkbox, and read back out
  of the dialog — and consulted by nothing. `settings.h` recorded the gap honestly in a comment
  *and* claimed the value seeded new per-app policies, which it also did not. A control that
  changes no behaviour is worse than an absent one, because it looks like it works.
* **The new variant: a flag set on an object the caller throws away.** `FilterOutcome::dismiss_review`
  was set in `InputFilter::on_event` before the `switch` that delegates to `on_key_event` — which
  returns *its own* outcome. The write went to a local nobody read. Not "implemented on one side
  and never called": called correctly, computed correctly, and dropped in the four lines between.
  It shipped, and the symptom was exactly what the operator reported — the pill dismissed on a
  click and sat through any amount of typing. The mouse half worked because it happened to be
  written inside `on_mouse_event`, on the far side of the same delegation.

The habit that catches the first two is §8's own: **check which entry points the product calls.**
The third needs a different one — *follow the value to its reader*, not just the call to its
definition — and it is the reason the tests for it assert the flag on the value `on_event`
**returns** rather than on anything inside it.

Earlier instances:

* `min_confidence`, `is_usable()` and `current_fallback()` all existed, were tested, and were
  called from nowhere in `src/` — the degradation rule was implemented *nowhere*.
* `note_session_ended()` was called from one of the two paths a session can end through, so an
  abort left the hotkey permanently dead.
* `o.error` — the session machine's full diagnosis — was built on every error and read by nothing.

**2026-08-21, and the `31 byte reason` line finally paid for itself.** The first real dictation
with `windows-speech` put *"committed text contradicted"* on the pill before a word was spoken. It
was neither. The press landed 1 s into a 4.2 s model load, `session.cpp:652` correctly refused it —
and then two things that had been wrong all along became visible:

* **The message was a fixed string.** All seven raisers of `Effect::SurfaceWarning` reached the
  overlay as `"committed text was contradicted"` (31 bytes — *that* is what the count means), so a
  refusal during a model load was reported as a stability violation. Only two of the three
  occurrences in the entire log are the thing the message names. `refusal()`'s own comment in
  session.h says it exists so the user is not left guessing; nothing had ever read it.
* **`begin_dictation` opened the microphone before asking, and ignored the answer.** `apply()`
  returns void, so `Verdict::Refused` fell through to `set_tray_state(Listening)` and
  `KD_LOG_INFO("dictation started")` for a session with no `start` on the wire. Hence the
  `no 'stopped' within 5000 ms` that followed: nothing was ever started to be stopped.

**Neither bug was new. What was new was a model slow enough to reach them.** Every other backend
reports ready in 0–77 ms; `windows-speech` takes 3140–4155 ms, so the window went from
unhittable to the default experience. A latency change turned two latent defects into the first
thing the operator saw. Fixed: the press is now *held* with the loading state showing and started
when the machine itself reports IDLE, the microphone is not opened until the activation is
accepted, and the overlay is given the real reason.

**2026-08-21, §3d, and it is the `31 byte reason` habit exactly.** The streaming API was written up
as disqualified because a segment "repeated across the seam". The segment's own `off=7960
dur=1120` was printed on the same line as the text, the speech in that clip ended at 7555 ms, and
comparing the two numbers is the whole refutation: it was not a repeat, it was the silence
hallucination this same section documents three paragraphs earlier. **The evidence was in the
output being read for evidence**, one field to the left of the text that was read instead — and it
was then written into three files as a settled finding, which is what made it expensive rather than
merely wrong. Caught only because the operator asked whether the design should have changed.

Its own twin: **one run is not a measurement.** The retracted claim came from a single clip, of a
single length, with padding the real product would never send. The correction came from a longer
clip run twice, byte-identical — which is what made the truncated-fragment artifact trustworthy
enough to write down.

And its measurement-side twin: **a harness that is not faithful to the product measures a different
function.** The Phase 0B row was measured three times before its numbers meant anything: first with
five of eleven scenarios arranged wrongly, then scoring against an incumbent anchor the product
always has and the probe never supplied. Both times the numbers were stable to the pixel and wrong.
Reproducibility says nothing about validity.

The habit that catches all of these: **before trusting that something works, check which entry
points the product actually calls.** A green test suite answers a different question.
