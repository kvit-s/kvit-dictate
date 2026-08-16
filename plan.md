# kvit-dictate — Implementation Plan (experimental phase)

A local, private replacement for Windows `Win+H`: press a hotkey, talk, text appears in
whatever field has focus. Target machine is this one — RTX 5090 (32 GB), 64 GB RAM,
28 logical cores, Windows 11 Pro.

The PRD is a research memo, not a spec. This plan turns it into buildable steps, with the
central constraint made explicit up front: **we do not yet know which ASR model wins, so
the first thing we build is the harness that decides it — not the app.**

---

## 0. Reading of the PRD, and one honest caveat

The PRD's recommendation is sound in shape:

> hotkey → mic → VAD → streaming ASR → optional second-pass correction → inject into focused window

and its ranking (streaming model first for latency, larger model for accuracy, Whisper as
baseline) is the right instinct.

**Caveat that must be resolved before anything is built:** the specific artifacts the PRD
names — `nvidia/nemotron-speech-streaming-en-0.6b`, Qwen3-ASR 0.6B/1.7B (arXiv 2601.21337),
and the 50-config comparison (arXiv 2604.14493) — are cited from a chat transcript with
`utm_source=chatgpt.com` on every link. I cannot confirm from my own knowledge that all
three exist exactly as described, and citations of that provenance are a common place for
plausible-but-wrong model names and arXiv IDs to appear. Phase 0 exists to check this. If a
model turns out not to exist, the architecture below is unaffected — only the contents of
the candidate list change, which is exactly why the candidate list is data, not code.

---

## 1. Architecture decision

### 1.1 Two processes, one contract

```
┌─────────────────────────────┐        ┌──────────────────────────────┐
│  kvit-dictate.exe (C++)     │        │  asr-server (Python, phase 1)│
│                             │        │                              │
│  • global hotkey            │ named  │  • model registry            │
│  • WASAPI mic capture       │ pipe   │  • VAD                       │
│  • VAD gate (optional)      │◄──────►│  • streaming decode          │
│  • floating overlay         │ binary │  • partial/final emission    │
│  • text commit engine       │ frames │  • optional 2nd-pass rescore │
│  • SendInput / clipboard    │ + JSON │                              │
│  • tray + settings          │        │  (swappable: NeMo, HF,       │
└─────────────────────────────┘        │   faster-whisper, sherpa)    │
                                       └──────────────────────────────┘
```

**Why split.** Every candidate model has a different runtime — NeMo, HuggingFace
`transformers`, `faster-whisper`/CTranslate2, ONNX. Binding all of those into a C++ process
is weeks of work *per model*, spent before we know which one we keep. In Python each new
candidate is a ~50-line adapter. The split lets us run the model bake-off in days instead of
months, and the C++ side never learns which model it is talking to.

**Why this is not a cop-out on "lightweight".** The C++ front-end is the part that must be
lightweight, and it stays lightweight — it is a few hundred KB of Win32 with no model code in
it. The Python process is a service, and Phase 5 replaces it in-process if measurements say
we should. Because the contract is fixed in Phase 1, that swap touches nothing on the C++ side.

### 1.2 Transport: duplex named pipe, message mode

`\\.\pipe\kvit-dictate`. Not a TCP socket:

- No Windows Firewall prompt on first run (a loopback listener triggers one; users read that
  as "this dictation app wants network access", which is exactly the wrong first impression
  for a privacy-motivated tool).
- No port collision, no port-file discovery dance.
- Pipe ACL restricts it to the current user for free.

Framing: 4-byte little-endian length + 1-byte type + payload.
Type `0x01` = JSON control message (UTF-8), type `0x02` = raw PCM audio (16 kHz, mono, s16le).
Audio flows client→server continuously while the hotkey is held; control flows both ways.

Control messages (v0):

```jsonc
// C++ → server
{"t":"hello","proto":1,"model":"nemotron-stream-0.6b"}
{"t":"start","sample_rate":16000,"context":["llama.cpp","Qwen3","RTX 5090"]}
{"t":"stop"}          // hotkey released — flush and finalize

// server → C++
{"t":"ready","model":"...","warm_ms":2140}
{"t":"partial","text":"can you change the llama dot cpp config","stable_chars":31,"seq":7}
{"t":"final","text":"Can you change the llama.cpp configuration…","seq":7,"latency_ms":180}
{"t":"error","msg":"..."}
```

`stable_chars` is the load-bearing field — see §1.4.

### 1.3 UI toolkit: plain Win32, not Qt

`..\kvit-notes` proves the Qt 6.10.1 / MSVC 2022 toolchain works here, and I looked at it
before deciding. But its UI needs are a rich text editor's; kvit-dictate's UI is:

1. a tray icon,
2. an **inline caret-anchored overlay** rendering the uncommitted text tail (§1.4a),
3. a settings dialog (hotkey, model, mic, injection mode).

Item 2 is the whole product, and it is not a widget — it is a layered, click-through window
positioned from an insertion point recovered by screen differencing (§1.4b) and painted with
DirectWrite in a fixed configured style. Qt contributes
nothing to that and costs ~45 MB (Quick) or ~25 MB (Widgets) to not contribute it. Plain
Win32 + Direct2D/DirectWrite is a few hundred KB, starts instantly, and puts nothing between
us and the APIs this app is *made of* (`WH_KEYBOARD_LL`, `WH_MOUSE_LL`, WASAPI, `SendInput`,
`BitBlt`).

Window flags, all four load-bearing:
`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT`.
`WS_EX_NOACTIVATE` — if the overlay takes focus, the target loses it and injection lands in
our own window. `WS_EX_TRANSPARENT` — clicks pass through to the app underneath, which the
user must be able to keep using while the overlay floats over their text.

Process must be `PROCESS_PER_MONITOR_DPI_AWARE_V2`: the target window can be on a different
monitor at a different scale, and caret rectangles come back in physical pixels.

Fallback: if the settings dialog and tray plumbing turn out to be a slog, build *only those*
in Qt Widgets and keep the hot path native. Decide at the end of Phase 3, not now.

### 1.4 The hard part nobody plans for: streaming commit

Streaming ASR revises what it already said. `Win+H` hides this; we cannot, because we are
typing into someone else's editor. Naïvely replaying revisions means emitting backspaces into
an arbitrary application — and backspace is not a safe key to send blind: in a browser it can
navigate, in an IDE it can dismiss autocomplete, in a terminal it can eat a prompt.

So the C++ side owns a **commit engine** with a strict rule: *only text the model is unlikely
to revise gets injected; everything else lives in the overlay only.*

- Server marks a prefix stable (`stable_chars`) when it has been unchanged across N
  consecutive partials (N tunable, start at 2) and is ≥ M chars behind the decode head.
- C++ injects only newly-stable prefix bytes. Append-only. Never backspaces into the target.
- The unstable tail renders in the overlay, positioned at the caret and styled to match the
  target field (§1.4a), so the user sees the model thinking without the document churning.
- On `final`, the remaining tail is injected in one shot and the overlay clears.

### 1.4a Inline overlay — the mechanism that makes this feel like `Win+H`

`Win+H` commits periodically and draws the uncommitted remainder as an inline overlay that
starts at the end of the committed text and mimics the input box's font and colours.

That detail is the trick, and it reframes §1.4. If the pending text is rendered *at the
caret, in the target's own font and colour*, the user cannot tell committed text from pending
text — it reads as one continuous stream of words appearing as they speak. Commit timing
stops being a perceived-latency tradeoff and becomes an implementation detail. A floating
status pill somewhere else on screen would not do this: the eye has to travel, and the seam
between "text in the pill" and "text in the document" is exactly what makes a dictation tool
feel laggy.

Consequences for the plan:

- **Commit policy can be time-based, not just stability-based.** Add `periodic-Nms` to the
  experiment matrix (§4) alongside `stable-N`. With the overlay absorbing the churn, periodic
  commits are viable and simpler.
- **Flicker rate (§Phase 2) demotes from UX metric to correctness metric.** It no longer
  predicts perceived quality — the overlay hides revision. It still bounds how much text we
  can commit before the model would have changed it, i.e. how wrong the final document gets.
- **Locating the insertion point becomes a hard dependency**, and it is the riskiest
  component in the app (§1.4b). Note this is the *only* thing that must be recovered from the
  target — the overlay's appearance is fixed in settings, not sampled from the app.

**Stop conditions match `Win+H`:** any real click or keystroke during dictation commits what
we have and stops. The low-level keyboard hook already sees every key; add `WH_MOUSE_LL` for
clicks. Both must filter `LLKHF_INJECTED` / `LLMHF_INJECTED` so our own `SendInput` traffic
never stops the session it is producing. See §1.4c for silence.

### 1.4c Two different silence timeouts — don't conflate them

Long thinking pauses mid-dictation must not end the session. `Win+H` handles this badly: it
auto-stops after a short silence and exposes **no setting to change it**, which is one of the
most common complaints about it. (I don't have a reliable figure for its exact default and
won't invent one — the point that matters is that it's short and not configurable.)

The fix is to recognise these are two unrelated timers that a naive implementation merges:

| Timer | Default | Purpose |
|---|---|---|
| **Endpoint silence** | ~0.8 s | Ends an *utterance* for the model — triggers finalisation, punctuation, sentence boundary. Purely an ASR concern; the session continues. |
| **Session silence** | **generous — 120 s, configurable, `never` permitted** | How long the session survives with no speech at all. |

A minute-long pause therefore costs nothing: the utterance endpoints after 0.8 s and finalises
cleanly, the session stays open, and resuming speech simply starts a new utterance appended
after a space.

**A long session timeout is safe here specifically because of the stop-on-input rule
(§1.4a).** Clicking anywhere or pressing any key already ends the session, which covers
essentially every real "I'm done" moment. That makes an idle session harmless and removes the
usual reason to time out aggressively. `never` is a legitimate setting, not a foot-gun.

**Gate the model with VAD during silence.** Do not keep feeding silence to a streaming model
across a 60-second pause: several ASR models hallucinate on long silence — `wan-dancer`'s own
`notes/transcribe.py` carries a comment about Whisper emitting "Thank you." / "Bye" over
trailing silence, with timestamps past the end of the audio. During VAD-detected silence, stop
feeding the decoder and reset its state at the endpoint; resume on speech onset.

The mic stays open across a long pause, so the tray icon and overlay must show listening state
unambiguously — an idle-but-hot mic the user has forgotten about is the one real cost of a
generous timeout.

### 1.4b Finding the insertion point: screen differencing, no accessibility APIs

The obvious design is a tiered probe — UI Automation first, `GetGUIThreadInfo` next, pixel
sampling as the last resort. **Rejected.** One universal path is better here, and pixel
differencing is the one that works everywhere.

Reasons, in order of weight:

1. **It is the only path that reaches WSL.** WSLg apps are X11/Wayland clients composited
   into Windows windows through an RDP pipe. They expose no UIA text pattern at all — which
   is precisely why `Win+H` refuses to dictate into them. They *are* real Windows windows, so
   `SendInput` reaches them and the desktop DC captures them. Pixel sampling therefore works
   where `Win+H` structurally cannot. That is a capability `Win+H` does not have, not parity.
2. **It deletes the worst threading hazard in the app.** UIA is cross-process COM; a single
   call can block for seconds on a busy target — the same target receiving our synthetic
   keystrokes. A screen `BitBlt` has bounded cost and no cross-process dependency.
3. **A tiered design must be tested and tuned per tier, per app.** Three code paths with
   different accuracy and different failure modes, and the rarely-hit tiers are the ones that
   rot. One path is exercised by every single dictation.
4. Games, Java/Swing, RDP sessions and Electron all collapse from "special cases" into
   "normal".

**The overlay's style is fixed, not sampled.** A single appearance configured once in
settings — font, size, colours — used in every application. Style matching was never worth
what it cost: font family is unrecoverable from pixels anyway, colour sampling breaks on
gradients and themed backgrounds, and a consistent look actually *reads better* because the
pending text becomes visually identifiable as pending. This deletes the whole
sampling-and-calibration layer.

**So the problem reduces to exactly one thing: where is the insertion point on screen?**

**Blink differencing is not the answer** — carets frequently do not blink. Windows Terminal
can be configured with a steady cursor, many editors default to steady, `GetCaretBlinkTime()`
can return `INFINITE` system-wide, and a focused-but-idle app may not draw a caret at all.
Any mechanism that requires blinking fails on a large fraction of real targets.

**Primary mechanism: commit differencing.** We do not need the caret to blink, because *we
are the thing making text appear.* Every commit is a probe we were going to send anyway:

1. Capture the foreground window's client region from the desktop DC (`BitBlt` from
   `GetDC(NULL)` — post-composition, so it sees Chromium, WSLg, and everything else DWM draws).
2. Inject the committed chunk (§1.5).
3. Capture again once the app has repainted.
4. Diff. The changed region *is* the text we just wrote. Its **right edge is the new insertion
   point**; its height is the line height.

This is strictly better than blink detection on every axis that matters:

- **No blink required**, so steady-cursor terminals and editors work.
- **Self-correcting.** Every commit re-anchors from scratch, so scrolling, window moves, line
  wraps, and the user clicking elsewhere all resolve themselves within one commit
  (~500 ms) instead of needing separate tracking.
- **Correct by construction.** The overlay renders pending text starting at the end of
  committed text — which is precisely what the diff's right edge gives us. No inference.
- **Free failure detection.** An empty diff means the injection produced no visible change,
  i.e. it silently failed. That is exactly the signal §1.5 needs to decide `SendInput` vs
  clipboard per app, and we get it without a separate mechanism.

**Bootstrap — before the first commit there is no anchor.** In order:

1. `GetGUIThreadInfo().rcCaret` — free, synchronous, no capture. Correct for classic Win32
   edit controls, empty for apps that draw their own caret. Try it because it costs nothing.

2. **The last left-click before the hotkey.** How a text position gets chosen, almost always,
   is that the user clicks there. So cache the most recent left-button-down point and use it
   as the initial anchor.

   This costs nothing to add: `WH_MOUSE_LL` is already installed for stop-on-input (§1.4a),
   so the clicks are already flowing through a hook we own. Store screen coordinates plus
   `WindowFromPoint` at click time. It is pure coordinates — no API dependency — so it works
   identically in WSLg, terminals, Electron, and games.

   Validity rules, all cheap:
   - The click's window must still be the foreground window; otherwise discard.
   - **Left-button only** — right-click opens a context menu and middle-click pastes on X11.
   - **Invalidate on mouse wheel.** Scrolling moves the caret on screen without any click, and
     the hook already sees wheel events.
   - Keyboard navigation since the click (arrows, Tab, Home/End) marks the anchor
     *low-confidence* rather than discarding it — a roughly-right anchor still beats no
     overlay, and it self-corrects at the first commit.

   Known-imprecise cases, all self-correcting within one commit: clicking past the end of a
   line (the caret snaps to line end, potentially far left of the click), clicking into an
   empty field (caret goes to field start), and clicking merely to focus a window. Vertical
   placement is the reliable part — the click lands within the target line — so anchor the
   overlay's line box on the click Y and treat the click X as a hint.

3. **Otherwise show no overlay until the first commit lands** — what `Win+H` does. The first
   words arrive in ~300 ms.

Step 2 is what makes the overlay feel instant, and its accuracy matters less than its
existence: appearing immediately in roughly the right place reads as responsive, and the
commit-diff correction at ~500 ms is largely invisible because the pending text is being
redrawn at that same instant anyway (the committed prefix leaves the overlay). **Snap, don't
animate** — a silent reposition during a content change goes unnoticed; a slide draws the eye
to the error.

**Capture cost.** Full client-rect capture only for bootstrap and re-acquire; once anchored,
capture a band around the expected region (last anchor ± a couple of lines). At ~2 commits/s
this is negligible on this machine.

**Failure modes:**

- **Busy background** (video, animated UI) pollutes the diff. Mitigate by diffing across the
  narrowest possible window around the injection, and by preferring the changed component
  nearest the previous anchor. If nothing is confident, keep the previous anchor — a slightly
  stale overlay position is far better than a jumping one.
- **HDR / multi-plane overlay** can make `BitBlt` return black. Detect an all-black capture
  and fall back to `Windows.Graphics.Capture`, which is DWM-composited and immune.
- **App renders asynchronously** (Electron, WSLg over RDP) so the "after" capture may precede
  the repaint. Poll the diff for a few frames with a short deadline rather than capturing once.

**Threading:** capture and diff run on a worker thread; results reach the paint path as plain
structs. No COM, no cross-process calls, no MTA apartment.

**Occlusion: not a concern.** The overlay may cover autocomplete popups and tooltips while
dictating; that is acceptable and needs no special handling.

**Second-pass correction is deferred.** The PRD's "Nemotron then Qwen" two-stage idea, and
the UI/UX questions it raises about rewriting already-committed text, are out of scope until
single-model streaming is proven end-to-end. Prove Nemotron alone first, measure what it
actually gets wrong, then decide whether a second pass is worth its complexity. Noted in §4
as a deferred variable, not a Phase 2 experiment.

### 1.5 Text injection strategy

**Both mechanisms ship in Phase 4, selectable in settings.** Neither is obviously better and
the answer is likely per-app, so the plan is to build both and decide from real use rather
than from reasoning:

1. **`SendInput` with `KEYEVENTF_UNICODE`** — universal, works in Win32/UWP/Electron/games.
   Slow for long text (~1–2 ms/char with pacing), and some apps drop fast synthetic input.
2. **Clipboard + `Ctrl+V`** — fast and atomic, but clobbers the clipboard (save/restore, and
   accept that clipboard-manager tools will still see it) and fails where `Ctrl+V` is bound
   elsewhere (terminals want `Ctrl+Shift+V`).

UI Automation `TextPattern`/`ValuePattern` is **not** implemented — it is absent in exactly
the targets that matter most here (WSLg, terminals), and §1.4b already commits to a no-UIA
design.

Settings: a global default plus per-app overrides, since the WSLg question below will
probably force a split. Start with `SendInput` as the default because it is the correctness
baseline, and record failures during dogfooding.

**WSLg needs an early test.** `SendInput` reaches WSLg windows as ordinary Windows input, but
that input is translated to X11 keysyms across the RDP pipe, and it is genuinely unclear
whether `KEYEVENTF_UNICODE` survives that translation intact. Clipboard paste is the more
likely path there (WSLg shares the clipboard by design). This is a concrete Phase 3 test, not
a Phase 4 detail — if `SendInput` fails into WSL, the per-app override mechanism becomes
mandatory rather than nice-to-have.

Capture the target `HWND` via `GetForegroundWindow()` **at hotkey-press**, and validate it is
still foreground before each injection — never inject into whatever happens to be focused later.

### 1.6 Hotkey

**Default: `Win+Y`, toggle semantics, exactly like `Win+H`.**

Press to start listening; it stops on silence (VAD), or on any click or keystroke (§1.4a).
No push-to-talk hold — the `Win+H` gesture is the target and hold-to-talk is a different
product.

`Win+Y` is nominally the Mixed Reality toggle, a feature that is effectively dead on a desktop
without a headset, so taking it costs nothing real.

**Mechanism.** `RegisterHotKey` refuses shell-owned combos outright, so it cannot bind
`Win+Y`. A low-level keyboard hook (`WH_KEYBOARD_LL`) on a dedicated thread sees keys *before*
the shell's hotkey processing, so returning `1` without calling `CallNextHookEx` swallows the
combo before Explorer ever sees it. (`Ctrl+Alt+Del` and `Win+L` are the only combos immune to
this — they route through a protected path.) The hook callback must never block: a slow hook
gets silently unhooked by Windows and the hotkey dies with no error.

**The one wrinkle with any `Win+<key>` binding.** Once we swallow the `Y`, Windows sees a Win
press with no intervening key and pops the Start menu when Win is released. The standard fix
is to inject a harmless keystroke (a bare `VK_CONTROL` tap) before the Win key-up so the
shell considers the chord consumed. It works, but it is a hack against undocumented shell
behaviour and is a plausible casualty of a future Windows update — so it goes behind a single
well-commented function with a test, and the settings dialog keeps non-Win alternatives one
click away.

**Settings offers:**

| Binding | Notes |
|---|---|
| `Win+Y` | **default** — shell-owned but by a dead feature; needs the Start-menu fix above |
| `Win+J` / `Win+O` | same class — Windows tips, orientation lock |
| `F13`–`F24` | completely free, no Win-key hack at all; available if your keyboard can emit them |
| `Ctrl+Win+Space` | chord, no shell conflict |
| Right `Ctrl` / right `Alt` / `CapsLock` | single-key options, no Start-menu problem; AltGr matters on non-US layouts |

---

## 2. Repository layout

```
kvit-dictate/
├─ prd.md
├─ plan.md
├─ CMakeLists.txt              # C++ app only
├─ CMakePresets.json           # mirror kvit-notes: windows-msvc-release
├─ win-build.bat               # no WSL-sync step; this repo is Windows-native
├─ src/
│  ├─ main.cpp                 # tray, message loop, lifecycle
│  ├─ hotkey.{h,cpp}           # WH_KEYBOARD_LL (Win+Y) + WH_MOUSE_LL stop-on-input,
│  │                           #   also caches last left-click as bootstrap anchor
│  ├─ anchor_tracker.{h,cpp}   # commit-diff screen capture → insertion point
│  ├─ audio_capture.{h,cpp}    # WASAPI shared-mode → 16 kHz mono s16
│  ├─ ipc_client.{h,cpp}       # named pipe, framing, reconnect
│  ├─ commit_engine.{h,cpp}    # stable-prefix diff → injection ops  ← unit-tested
│  ├─ injector.{h,cpp}         # SendInput / clipboard / UIA
│  ├─ overlay.{h,cpp}          # layered window, D2D/DWrite
│  ├─ settings.{h,cpp}         # JSON in %LOCALAPPDATA%\kvit-dictate
│  └─ server_host.{h,cpp}      # spawn/supervise the ASR process
├─ tests/                      # ctest; commit_engine + framing are pure logic
├─ envs/                       # only created when §5.1a forces it; gitignored
│  └─ .venv-onnx/              #   ONNX runtime, no torch — the preferred backend env
├─ server/                     # Python; runs under a venv chosen per backend
│  ├─ pyproject.toml
│  ├─ kvit_asr/
│  │  ├─ __main__.py           # pipe server + session state machine
│  │  ├─ protocol.py           # framing, mirrors ipc_client.cpp
│  │  ├─ vad.py                # Silero VAD (ONNX)
│  │  ├─ registry.py           # name → adapter
│  │  └─ backends/
│  │     ├─ base.py            # StreamingASR: feed(pcm) → partial|final
│  │     ├─ whisper_ct2.py     # faster-whisper — baseline, known-good
│  │     ├─ nemo_stream.py     # NeMo streaming (Nemotron/Parakeet/Canary)
│  │     ├─ qwen3_asr.py       # HF transformers
│  │     └─ sherpa_onnx.py     # ONNX — the C++-portable path
└─ bench/
   ├─ corpus/                  # audio + reference transcripts (gitignored: large)
   ├─ record.py                # capture own-voice reads via same WASAPI path
   ├─ run_matrix.py            # model × mode × chunk-size sweep
   ├─ metrics.py               # WER, term recall, latency, flicker
   └─ results/                 # one JSON per run; committed
```

---

## 3. Phases

Each phase ends with something runnable. Phases 1–2 are the experiment; 3–4 are the app;
5 is optional consolidation.

### Phase 0 — Verify the landscape (½ day)

Before writing an adapter for a model that may not exist:

1. Resolve each PRD claim against the actual Hugging Face model page / arXiv listing:
   `nvidia/nemotron-speech-streaming-en-0.6b`, Qwen3-ASR 0.6B/1.7B, arXiv 2601.21337,
   arXiv 2604.14493.
2. For each that resolves: record license, param count, runtime dependency, streaming
   support, and **whether a published ONNX export exists**. That last column now decides two
   things at once — Phase 5's in-process C++ feasibility *and* whether we can avoid ever
   installing NeMo (§5.1a).
3. Backfill the candidate list with models known to be real regardless of the above:
   `nvidia/parakeet-tdt-0.6b-v2` (NeMo, strong English, real streaming support),
   `faster-whisper` large-v3 and distil-large-v3 (baseline), and sherpa-onnx's prebuilt
   streaming transducers.
4. ~~Confirm PyTorch works on Blackwell (sm_120).~~ **Already satisfied** — see §5.
   `..\wan-dancer\.venv` has torch 2.10.0+cu130 with `sm_120` in `get_arch_list()` and the
   5090 detected. This was the biggest environment risk in the original plan and it is gone.
   Remaining check is a real forward pass per *backend*, not per torch install.

**Exit:** `bench/CANDIDATES.md` — a table of models that provably exist, load, and run on
this GPU. Everything downstream reads from this list.

### Phase 1 — Offline bake-off (2–3 days)

No app, no hotkey, no UI. Just: which model is best at *our* audio?

1. **Corpus.** Three sets, ~20 min total:
   - `tech/` — you reading 40–60 technical sentences (the PRD's `llama.cpp` / Qwen3.6 /
     speculative MTP / RTX 5090 sentence is exactly right; add file paths, CLI flags,
     git SHAs, model numbers, acronyms). **This is the set that decides the winner** —
     public benchmarks do not contain this vocabulary, and it is where Win+H actually fails.
   - `hard/` — same speaker, adverse: fan noise, music, fast speech, trailing-off ends,
     mid-sentence self-correction.
   - `clean/` — a LibriSpeech test-clean slice, purely as a sanity check that a backend is
     wired correctly (a broken adapter shows up here as an absurd WER).
   Record `tech/` and `hard/` through the *same WASAPI path the app will use*, so the
   bake-off is not silently testing a different mic chain than production.
2. **Adapters.** Implement `StreamingASR` for every Phase-0 survivor. Batch mode first.
3. **Metrics** (`bench/metrics.py`):
   - WER / CER, normalized (Whisper-style text normalizer) **and** raw — raw is what matters
     for `llama.cpp` vs `llama dot cpp`, and normalization hides exactly the errors we care about.
   - **Technical-term recall/F1** against a hand-written keyword list per utterance. A model
     that gets 97 % WER but drops every identifier is useless here; this metric, not WER,
     is the headline number.
   - RTF and peak VRAM.
4. **Exit:** `bench/results/phase1.json` + a short table. Pick 2–3 finalists.

### Phase 2 — Streaming + latency bake-off (2–3 days)

The finalists, now measured on the axis that actually distinguishes a dictation tool.

1. Wrap each finalist in true chunked streaming (chunk sizes 80/160/320/640 ms where the
   model allows).
2. Add VAD (Silero ONNX) — utterance endpointing, and decoder gating during silence so a
   long pause cannot provoke hallucinated text (§1.4c). Measure endpoint-silence threshold:
   too short chops sentences, too long delays finalisation.
3. **Latency metrics:**
   - *first-partial latency* — speech onset → first text on screen.
   - *finalization latency* — speech end → `final`.
   - *commit lag* — speech onset → that word becoming **stable** (this is the number the
     user feels, and it is strictly worse than first-partial latency).
   - **flicker rate** — chars emitted in a partial that a later partial revised, per 100
     chars. Low flicker means we can commit aggressively; high flicker forces Mode B.
     This is the metric that decides §1.4, and it is the one no public benchmark reports.
4. ~~Second-pass experiment.~~ **Deferred** — see §4.
5. **Exit:** one winning (model, chunk size, commit policy) tuple, with
   numbers behind it. Targets to aim at: first partial < 300 ms, finalize < 500 ms after
   speech end, flicker < 2 %.

### Phase 3 — C++ app skeleton (3–4 days)

Now build the thing. Server stays Python; the winner from Phase 2 is hardcoded as default.

1. CMake + preset mirroring `kvit-notes` (`windows-msvc-release`, VS 17 2022, x64), minus
   the WSL mirror-sync — this repo lives on Windows natively.
2. `hotkey` (`Win+Y`, §1.6) → `audio_capture` → `ipc_client` → console output. No UI. Prove
   end-to-end audio-to-text through the pipe.
2b. **WSLg injection spike** — one throwaway binary that `SendInput`s Unicode text into a
   WSLg window and reports whether it arrives intact, then the same via clipboard paste
   (§1.5). Cheap, and it decides whether per-app injection overrides are mandatory. Do it
   here, not in Phase 4.
3. `server_host`: spawn the Python server as a child, redirect its stderr to a log, restart
   on crash, kill on exit (job object, so a crashed app never orphans a model holding 4 GB
   of VRAM).
4. `commit_engine` with unit tests — pure logic, no Win32, so it is fully testable under
   ctest. Feed it recorded partial sequences from Phase 2 and assert the injection op stream.
5. **Exit:** talk into a console window, see committed text scroll.

### Phase 4 — Real app (4–5 days)

1. `injector` — **both** methods (SendInput and clipboard), per-app override, target-HWND
   validation (§1.5).
2. `anchor_tracker` — commit-differencing insertion-point probe, worker thread (§1.4b).
   **Build and de-risk this first; it is the highest-risk component in the app.** Ship a
   standalone probe tool that injects a known string and reports whether the diff resolved a
   plausible insertion point, per app. Run it against the real target set: VS Code, Chrome,
   Windows Terminal (**steady cursor, not blinking**), **a WSLg window**, Slack, Explorer's
   search box, Office, and `kvit-notes` itself. The probe also reports **click-anchor
   agreement** — how far the last-click bootstrap anchor sat from the commit-diff anchor, per
   app — which is what tells us whether the bootstrap is worth keeping and where it is worst.
3. `overlay` — layered, click-through, anchored at the tracked insertion point, drawn in the
   single configured style (§1.4b). Floating-pill fallback where no anchor resolves. Level
   meter and state indicator live in the tray icon, not over the user's text.
4. Tray icon, settings dialog, autostart, model picker driven by the server's registry.
5. Warm-start: load the model at app start or on first hotkey with an explicit "warming"
   tray state, so the first dictation of the session is not the slow one.
6. Failure UX: server dead, no mic, model OOM, target window vanished mid-utterance,
   caret geometry lost mid-sentence (scroll, window move, popup steals the spot).
7. **Exit:** daily-drivable. Dogfood it for a week and log every miss into `bench/corpus/tech/`
   — the corpus should grow from real failures, which is the only way it stays honest.

### Phase 5 — Optional: collapse to one process

Only if Phase 2/4 numbers justify it. Triggers: IPC overhead is a measurable share of commit
lag, or the ~1.5 GB Python RSS and multi-second cold start become the top complaint.

Path: if the winner has ONNX weights, `sherpa-onnx` or ONNX Runtime (DirectML/CUDA EP) links
into the C++ process directly. If the winner is Whisper-family, `whisper.cpp` with CUDA.
If the winner is NeMo-only with no ONNX export — keep the sidecar; that is a fine outcome,
and §1.1 exists precisely so this decision stays cheap.

---

## 4. Experiment matrix

```
model         ∈ Phase-0 survivors                    (~4–6)
chunk_ms      ∈ {80, 160, 320, 640}                  (Phase 2 only)
commit_policy ∈ {stable-2, stable-3, periodic-500ms, periodic-1000ms, finalize-only}
inject_method ∈ {SendInput, clipboard}                (Phase 4, per-app, both shipped)
```

**Deferred:** `second_pass ∈ {llm-rewrite, asr-rescore}`. Prove single-model streaming
end-to-end with Nemotron first and measure what it actually gets wrong; only then decide
whether a second pass earns its complexity and the UX questions it drags in (§1.4b).

Full cross-product is wasteful. Sequential elimination: Phase 1 cuts on accuracy (model
only), Phase 2 sweeps chunk × commit × second_pass on survivors only, Phase 4 A/B/Cs by hand
on the single winner. `bench/run_matrix.py` writes one JSON per cell so results accumulate
across days and nothing is re-run by accident.

---

## 5. Build & experiment environment (verified on this machine, 2026-08-16)

### 5.1 Python — reuse `..\wan-dancer\.venv`, but not for everything

That venv is a working GPU stack and reusing it removes the plan's biggest unknown:

```
python 3.12.9    torch 2.10.0+cu130    cuda_build 13.0    available True
device NVIDIA GeForce RTX 5090   capability (12,0) = sm_120
arch_list ['sm_75','sm_80','sm_86','sm_90','sm_100','sm_120']
transformers 5.14.1  numpy 2.4.4  librosa 0.11.0  soundfile 0.14.0  torchaudio 2.11.0+cu130
```

`sm_120` is compiled in — Blackwell is natively supported, no PTX-JIT fallback. Phase 0's
torch gate is closed before it opened.

Two corrections to the assumption behind reusing it, both of which change the plan:

**(a) Whisper is not installed as a package.** `wan-dancer\notes\transcribe.py` runs Whisper
through the `transformers` ASR pipeline against a cached `openai/whisper-large-v3-turbo`
checkpoint. That is genuinely useful — it is a proven-working Whisper baseline on this GPU,
and Phase 1's baseline adapter can be a near-copy of that script. But `faster-whisper`,
`openai-whisper`, and `whisper.cpp` are all still absent, and each pulls its own stack.

**(b) That venv must not be used for NeMo.** It contains
`sageattention 2.2.0+cu130torch2.10.0andhigher.post6`, compiled against that exact torch, and
it is what wan-dancer's H3 video pipeline runs on. NeMo pins older `transformers` and its own
torch; if pip resolves by downgrading, sageattention stops loading and the user's video
pipeline breaks — at render time, not install time. **Not a risk worth taking to save a few
GB of disk.**

### 5.1a No new PyTorch environments unless forced

Scanned `C:\apps`, `D:\projects` and the conda envs (2026-08-16). Exactly two torch installs
exist on this machine:

| Environment | torch | Usable for ASR? |
|---|---|---|
| `D:\projects\wan-dancer\.venv` | 2.10.0+cu130, sm_120 | **yes** — the one good GPU env |
| `C:\Users\sk\miniconda3\envs\marketenv` | 2.9.1+**cpu** | no — CPU-only build |

So there is no existing NeMo-capable environment to borrow, and a second GPU torch install
would cost ~4 GB. The plan avoids creating one, in this order:

1. **Prefer ONNX — it needs no torch at all.** `onnxruntime-gpu` + `sherpa-onnx` is a few
   hundred MB with no PyTorch anywhere. This is not a compromise: it is *also* the Phase 5
   path into the C++ process, so doing the streaming experiments on ONNX from the start means
   the winner is already in the runtime we would eventually want. Phase 0 gains an explicit
   check — **does a published ONNX export exist for each candidate?** — and a "yes" there
   removes the entire NeMo question.
2. **If a model is torch-only, dry-run before installing.** `uv pip install --dry-run
   nemo_toolkit[asr]` against the existing venv resolves the full dependency graph without
   touching anything. Read whether it moves torch / transformers / numpy. If it resolves
   clean, install in place and no new env is needed.
3. **Only if the dry-run shows the frozen set moving** does a sibling venv get created — and
   only for that one backend.

If a sibling does become necessary, set `UV_CACHE_DIR` to a directory on **D:** first. uv
hardlinks packages out of its cache into venvs, but hardlinks cannot cross volumes — with the
cache on C: and venvs on D: every package is a full copy. Same-volume, a second env sharing
versions with the first costs almost nothing.

Weights are shared regardless: `HF_HOME=c:\apps\llama-swap\gguf\hf` is set globally, so no
model is ever downloaded twice no matter how many venvs exist.

| Backend | Environment |
|---|---|
| Whisper via `transformers` (baseline) | `..\wan-dancer\.venv` — proven, zero new deps |
| `sherpa-onnx` / ONNX Runtime | `envs\.venv-onnx` — **no torch**, preferred for all streaming candidates |
| Qwen3-ASR, if a `transformers` model not requiring `transformers<5` | `..\wan-dancer\.venv` after a dry-run |
| NeMo, *only if no ONNX export exists* | dry-run first; sibling env only if it would move the frozen set |

`server/` takes a `--python` argument so `server_host.cpp` launches whichever interpreter the
selected model needs — the C++ side already doesn't know what model it's talking to (§1.1),
and this keeps it that way.

**Ledger requirement:** `..\wan-dancer\asr-models.md` records every model and package added
for this work, plus the frozen-version set and a post-install check that fails loudly if
torch/transformers/numpy moved. Written, with the pre-existing baseline captured. Update it
in the same commit as any install — a ledger backfilled from memory is worthless.

Already cached and free to use: `openai/whisper-large-v3-turbo` (1.5 GB),
`Systran/faster-whisper-base.en` (141 MB), `ggerganov/whisper.cpp` ggml (42 MB).

Also present: llama-swap (`C:\apps\llama-swap`, OpenAI-compatible on `127.0.0.1:8080`, not
always running) serving local GGUF models including `Qwen3.6-27B-MTP` — which is the host for
Phase 2's second-pass LLM rewrite experiment. That experiment needs no new infrastructure,
and it can be run against the exact model the PRD's example sentence talks about.

### 5.2 C++

| Component | Status |
|---|---|
| Qt | 6.10.1 msvc2022_64 + 6.7.1 at `C:\Qt` — available, **not planned as a dependency** (§1.3) |
| Compiler | VS 2022 Community, MSVC 2022 |
| CMake | `C:\Program Files\CMake\bin\cmake.exe` (also VS-bundled). Ninja **not** on PATH — presets use the VS generator, as `kvit-notes` does |
| GPU / CUDA | RTX 5090 32 GB, driver 610.74; CUDA 12.9 + 12.1 toolkits (torch ships its own cu130 runtime) |
| RAM / CPU | 64 GB / 28 threads |

C++ standard: C++20, matching `kvit-notes`.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| PRD's models don't exist / aren't as described | Phase 0 gates everything; candidate list is data |
| ~~PyTorch has no sm_120 kernels~~ | **Closed** — `wan-dancer\.venv` torch 2.10.0+cu130 has sm_120 compiled in (§5.1) |
| An ASR install downgrades torch and breaks wan-dancer's video pipeline | Per-stack venvs; NeMo never enters the shared one; post-install check in `asr-models.md` §4 |
| Flicker too high to commit while streaming | Mode B (preview-then-commit) is the fallback and is still a good product |
| `SendInput` unreliable in some target apps | Clipboard fast path; per-app override in settings; log failures during dogfooding |
| Commit-diff fails to resolve an insertion point (busy background, async repaint) | Narrow diff window, prefer component nearest previous anchor, keep stale anchor over a jumping one; probe tool measures real per-app success before the design is committed to (Phase 4.2) |
| `BitBlt` returns black under HDR/MPO | Detect all-black capture, fall back to `Windows.Graphics.Capture` |
| ~~Font family unrecoverable from pixels~~ | **Moot** — overlay style is fixed in settings, not sampled (§1.4b) |
| Long pause kills the session, or model hallucinates over silence | Split endpoint (0.8 s) from session (120 s, `never` allowed) timeouts; VAD-gate the decoder during silence (§1.4c) |
| `SendInput` Unicode doesn't survive the WSLg RDP translation | Phase 3 spike decides it; clipboard path already shipping as the alternative |
| A second GPU torch env eats another 4 GB | ONNX-first (no torch); `uv pip --dry-run` before any install; `UV_CACHE_DIR` on D: for hardlinking (§5.1a) |
| Antivirus flags the keyboard hook + synthetic input | Expected — a hotkey-driven keystroke injector looks exactly like a keylogger to heuristics. Sign the binary early; test under Defender before Phase 4 ends |
| Python sidecar cold start ruins first use | Warm at app start; explicit "warming" overlay state |
| Scope creep into a transcription app | Non-goals, below |

**Non-goals for the experimental phase:** file transcription, diarization, multi-language,
cloud fallback, installer/auto-update, non-Windows. All are reasonable later; none inform the
model choice, which is the only question this phase exists to answer.

---

## 7. Milestone checklist

- [x] **M-1** Experiment env resolved — `wan-dancer\.venv` (torch 2.10.0+cu130, sm_120 ✓),
      per-stack venv policy, ledger at `..\wan-dancer\asr-models.md`
- [ ] **M0** `bench/CANDIDATES.md` — models verified to exist and run on sm_120
- [ ] **M1** Corpus recorded; offline WER + technical-term F1 for all candidates
- [ ] **M2** Streaming latency + flicker numbers; winning config chosen
- [ ] **M3** Console end-to-end: hotkey → mic → pipe → committed text
- [ ] **M4** Overlay, injection, tray, settings — daily-drivable
- [ ] **M5** One week of dogfooding; failures folded back into the corpus
- [ ] **M6** *(optional)* single-process C++ inference

---

## 8. Immediate next step

Phase 0, item 1 — resolve the four PRD citations against real sources, since roughly half the
plan's model list is currently taken on faith from a chat transcript. Say the word and I'll
run that verification and write `bench/CANDIDATES.md`.
