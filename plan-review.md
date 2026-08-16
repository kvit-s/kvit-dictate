# Review of `plan.md`

## Overall assessment

The plan has a strong product thesis and a sensible research-first instinct. In particular,
it correctly treats model selection as an empirical question, separates the replaceable ASR
runtime from the Windows front end, recognizes that streaming revisions make arbitrary text
injection dangerous, and defers second-pass rewriting until the basic experience works.

It is not ready to execute unchanged, however. The plan makes its least-proven Windows UI
mechanism—the caret-like inline overlay—a hard architectural dependency, but does not test it
until after roughly a week of model work. It also contains a contradiction at the center of
the commit algorithm: periodic commits can commit text the recognizer later revises, while the
stated safety invariant forbids repairing committed text. Finally, the IPC, focus, and failure
contracts are too loose for a program that injects text into another application.

Recommendation: approve Phase 0 model verification, but insert a Windows feasibility spike
and resolve the three blocking design issues below before starting the full bake-off.

## What is already strong

- The candidate list is data rather than an architectural commitment. Phase 0 is the right
  response to an unverified research memo.
- The C++ front end/Python model sidecar split is appropriate for experimentation. Keeping
  model-specific dependencies out of the UI process is more valuable now than optimizing
  cold start or RSS.
- The personal technical-speech corpus is much more relevant than choosing a model from a
  public WER table. Raw text and technical-term scoring are necessary additions to normalized
  WER.
- Separating utterance endpointing from session lifetime is correct and directly addresses
  long thinking pauses.
- Deferring second-pass correction avoids designing a destructive rewrite UX before the
  first-pass error distribution is known.
- Capturing the target at session start and refusing to inject after focus changes is the
  correct safety posture, though the exact target identity still needs definition.

## Blocking findings

### 1. Periodic commit conflicts with the append-only safety invariant

Sections 1.4 and 1.4a say that only stable text is injected and that committed text is never
repaired with backspaces. The experiment matrix then includes `periodic-500ms` and
`periodic-1000ms`, apparently allowing an unstable prefix to be committed on a timer. The
overlay can hide revisions only while text remains in the overlay; it cannot absorb a
revision to text already injected into the target.

The protocol has the same unresolved case: a `final` message can differ from the prefix that
the client has already committed. The plan does not say whether the server must preserve that
prefix, whether the client discards the corrected portion, or whether the session fails.

Required change:

- Define one non-negotiable invariant: every injected Unicode scalar sequence must remain a
  prefix of all later hypotheses and of the final result for that utterance.
- Make time-based policy affect *when an eligible stable prefix is flushed*, not which text
  is considered stable. If a deliberately speculative mode is desired, name it as unsafe and
  keep it out of the default experiment.
- Specify the stability algorithm precisely: longest common prefix over which hypotheses,
  minimum age/head distance, treatment of whitespace and punctuation, and reset behavior at
  an utterance boundary.
- Define what happens when a backend violates the committed-prefix invariant. The safe
  behavior is to preserve the document, show/log the unapplicable correction, and mark the
  backend/run as an irrevocable-error event.
- Measure **irrevocable committed error rate**, not just partial-text flicker. Flicker can be
  high with zero document corruption, or low while a single early wrong commit remains.
- Define offsets as Unicode code points or UTF-8 byte offsets end to end. `stable_chars`
  currently becomes “prefix bytes” in the prose, while Windows injection ultimately consumes
  UTF-16 code units. Never permit an offset to split a UTF-8 sequence or surrogate pair.

### 2. The highest-risk UI mechanism is tested too late and is overclaimed

The plan calls commit differencing “correct by construction” and “universal,” but the changed
region's right edge is not generally the insertion point. Counterexamples include:

- a commit containing only spaces or a newline;
- line wrapping, horizontal scrolling, or target auto-scroll;
- selections being replaced;
- right-to-left or bidirectional text;
- terminals with a block cursor;
- syntax highlighting or spellcheck repainting older text;
- autocomplete, animated content, caret blink, or the overlay itself contaminating the diff;
- password/protected fields and content intentionally excluded from capture.

An empty visual diff also does not prove injection failure: whitespace can be accepted without
changing pixels, and text may be accepted outside the visible region. Conversely, a non-empty
diff does not prove the expected text arrived. Injection success and anchor estimation must be
separate signals.

This is a product-feasibility risk, not a Phase 4 implementation detail. If the inline anchor
does not work reliably, the UX and commit policy both change. Spending 4–6 days selecting a
model first would optimize a component of a product whose defining interaction remains
unproven.

Required change:

- Move the standalone injection/anchor/overlay probe to immediately after factual Phase 0.
  It can use fixed text and does not need ASR.
- Test a scenario matrix, not just an app list: empty field, mid-line insertion, selection
  replacement, line wrap, newline, spaces, horizontal/vertical scroll, dark/light theme,
  autocomplete open, steady cursor, mixed DPI, HDR, WSLg, and an elevated target.
- Treat screen differencing as a best-effort anchor source until the measurements justify a
  stronger claim. Keep `GetGUIThreadInfo` and last click, but define a confidence score and a
  deterministic transition to the floating-pill or no-preview fallback.
- Exclude or hide the app's own overlay during captures and account for target-window movement
  between the before/after frames.
- Establish an explicit gate, such as exact injection in every supported target and anchor
  placement within one line height in at least 95% of supported scenarios. The precise
  threshold is a product decision, but it must exist before inline overlay is a dependency.
- Do not assume `Windows.Graphics.Capture` is a transparent fallback. Microsoft's documented
  flow includes support checks and user-facing capture/consent behavior; prototype the exact
  desktop API path and resulting UX during this spike.

### 3. The process and protocol contract is not safe enough yet

The wire examples are helpful but insufficient for asynchronous audio, reconnects, model
reloads, and multiple utterances. `seq: 7` is not defined, and there are no session or
utterance identifiers. A late `final` after a stop/restart could therefore be applied to the
wrong target. There is also no backpressure rule, maximum frame size, cancellation handshake,
or statement of whether partial text is cumulative or delta-based.

The claim that a named-pipe ACL restricts access to the current user “for free” is incorrect.
Microsoft documents that a pipe created with the default descriptor also grants read access
to Everyone and anonymous users. An explicit security descriptor is required. See
[Named Pipe Security and Access Rights](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights).

Required change:

- Add a versioned protocol specification before implementing either endpoint.
- Include `session_id`, `utterance_id`, monotonically increasing event/audio sequence numbers,
  audio sample position or monotonic timestamp, cumulative-vs-delta semantics, and explicit
  `stop`, `cancel`, `stopped`, `loading`, `ready`, and terminal error states.
- Define client behavior for duplicate, missing, late, and out-of-order messages and for a
  server disconnect after some text has been committed.
- Specify maximum control/audio frame sizes, bounded queues, and overload behavior. Dropping
  or delaying audio silently is not acceptable.
- Create the pipe with an explicit current-user DACL. Prefer a per-launch unpredictable pipe
  name passed directly to the child, or otherwise defend against pipe squatting and validate
  the peer/process relationship.
- Add cross-language protocol fixtures and tests for fragmented reads, combined frames,
  malformed lengths/JSON, Unicode boundaries, reconnect, cancel, and stale-session messages.

## Important design corrections

### Make the audio path real before recording the decision corpus

Phase 1 says the corpus will use the same WASAPI path as production, but the C++ WASAPI
capture implementation is not built until Phase 3 and `bench/record.py` is described as the
recorder. Those cannot literally be the same path as sequenced.

Build a minimal production capture/recording utility before collecting the personal corpus,
or explicitly accept that the corpus uses a different implementation. Specify native device
format discovery, channel mixing, resampling to 16 kHz, buffer discontinuities, timestamps,
default-device changes, and microphone selection. Preserve the original capture metadata so
backend bugs are distinguishable from resampling/capture bugs.

### Keep the shared `wan-dancer` environment read-only

Section 5.1 says NeMo must never enter the shared venv; section 5.1a then permits installing it
there if a dry run appears not to move selected packages; the risk table again says NeMo never
enters it. A dry run cannot guarantee that an added package will not break an untested import
or runtime path, and guarding only torch/transformers/numpy is too narrow.

Use the existing environment only to run its already-working Whisper baseline. Create locked,
backend-specific environments for every new runtime, even when versions appear compatible.
Disk use is a smaller and more reversible cost than corrupting another project's working GPU
stack. Keep the authoritative environment manifest/lock files in this repository; a ledger in
`../wan-dancer` may be a useful cross-reference but should not be required to reproduce this
project.

The multi-environment registry also needs clarification. A registry inside one Python server
cannot enumerate models available only under other interpreters. Put the model-to-interpreter
mapping in a small front-end manifest, then restart the sidecar under the selected backend
environment.

### Define how the model winner is selected

The plan collects good metrics but does not define the decision rule. “Pick 2–3 finalists”
and “one winning tuple” invite subjective selection after seeing results.

Before running the corpus:

- Create a development set and a held-out decision set; do not tune vocabulary, prompts,
  normalization, VAD, or stability policy on the held-out set.
- Define technical-term matching for case, punctuation, aliases, and compound identifiers.
- Correct “97% WER” in Phase 1; this presumably means 97% accuracy or 3% WER.
- Compare context-enabled and context-free modes separately. A backend without context support
  should not silently receive a different task.
- Replay streaming audio at real-time cadence and distinguish algorithmic delay, queueing,
  inference time, endpoint delay, and UI/commit delay. Measure warm and cold starts, RAM/VRAM,
  and repeated-run variance.
- Add silence-only, noise-only, and long-pause hallucination tests.
- State the selection rule in advance—for example, maximize held-out technical-term F1 among
  configurations meeting first-partial, finalization, irrevocable-error, and resource limits.
- Include a small manual Win+H comparison on the same sentences. It need not be a scientific
  benchmark, but the project should verify that its chosen system improves the failure mode it
  exists to replace.

### Expand target and privacy safety

`SendInput` is subject to UIPI and can inject only into equal- or lower-integrity targets;
Microsoft also notes that its return value does not identify UIPI as the cause. Elevated apps
therefore need an explicit unsupported/fallback state rather than generic diff-based failure
detection. See [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput).

Also define:

- target identity as top-level foreground window plus focused thread/control when obtainable;
- behavior for secure desktop, lock, UAC prompts, password fields, and remote sessions;
- whether a second hotkey press finalizes or cancels, and what happens to an unstable tail;
- exact handling of modifiers and key-up events so the activation chord does not immediately
  trigger stop-on-input;
- clipboard ownership checks before restoration, so a clipboard update made by the user or
  another app is never overwritten;
- a no-transcript/no-audio logging default, log retention, redaction, and whether committed
  benchmark JSON can contain private corpus text;
- runtime network policy. “Local” should mean no runtime telemetry or cloud fallback, not only
  local inference.

### Broaden verification beyond two pure-logic units

The stated tests cover framing and the commit engine, but the risky behavior lives at the
boundaries. Add tests for the session state machine, audio conversion and discontinuity,
VAD/endpoint transitions, child crash/restart, focus races, hook filtering of self-injected
events, clipboard preservation, DPI coordinate conversion, capture/diff fixtures, and a
small controlled target app containing classic edit, rich edit, multiline, and password
controls.

The Phase 4 estimate of 4–5 days is optimistic for hooks, capture fallback, Direct2D overlay,
two injection paths, settings, tray, autostart, supervision, and the full failure matrix.
Replace the calendar estimate with exit gates or add contingency; otherwise the schedule will
encourage declaring the riskiest paths “good enough” without evidence.

## Internal inconsistencies and cleanup

- Section 1.6 says the session “stops on silence (VAD),” while section 1.4c says endpoint
  silence finalizes only an utterance and session silence defaults to 120 seconds. Use the
  latter wording consistently.
- “Mode B (preview-then-commit)” appears in the risk table but is never defined. Define it as
  finalize-only injection or remove the label.
- The repository tree comments `injector.{h,cpp}` as “SendInput / clipboard / UIA,” although
  UIA is explicitly rejected.
- Section 4 says second pass is deferred, but its final paragraph says Phase 2 sweeps
  `second_pass`, and section 5.1 says a Phase 2 LLM rewrite experiment needs no infrastructure.
  Remove the stale Phase 2 references.
- The text alternates between proving “Nemotron alone” and selecting a winner only after Phase
  0/2. Use “the selected streaming finalist” unless Nemotron has already passed Phase 0.
- Reorder subsections 1.4b and 1.4c, and avoid claims such as “universal,” “strictly better,”
  and “correct by construction” until the probe data supports them.
- The model list mixes true-streaming systems with batch Whisper adapters. Record each
  backend's mode explicitly (`native-streaming`, `stateful-chunked`, or
  `overlapping-window/pseudo-streaming`) so latency comparisons are interpretable.

## Recommended phase order

1. **Phase 0A — source and environment verification.** Produce `CANDIDATES.md`, model
   licenses, runtime requirements, native streaming mode, and reproducible lock files. Do not
   install into the shared project venv.
2. **Phase 0B — Windows feasibility spike.** Build a fixed-text injector, focus guard,
   minimal overlay, and anchor probe. Run the full app/scenario matrix, including WSLg and an
   elevated target. Choose inline overlay as a supported optimization or adopt the floating
   preview/finalize-only fallback as the baseline.
3. **Phase 1 — production audio recorder and offline bake-off.** Build the actual capture and
   conversion path first, freeze development/held-out corpora and metric definitions, then
   reduce the candidate set.
4. **Phase 2 — protocol plus streaming bake-off.** Freeze the protocol and commit invariants,
   implement streaming adapters/VAD, and select using predetermined gates including
   irrevocable committed error.
5. **Phase 3 — minimal vertical product.** Hotkey → production audio → secured sidecar →
   safe focus-checked injection, with console/floating status and crash handling.
6. **Phase 4 — productization.** Add the measured anchor strategy, tray/settings/autostart,
   per-app policies, privacy controls, and failure UX. Publish a supported-target matrix.
7. **Phase 5 — dogfood and consolidate only from measurements.** Grow a separate regression
   corpus from real misses, then consider single-process inference or second-pass correction.

## Suggested definition of “daily-drivable”

M4 should not be a subjective exit. At minimum:

- no wrong-window injection in automated focus-race tests;
- exact Unicode injection for every declared supported target and an explicit failure state
  for unsupported/elevated targets;
- committed-prefix violations below a declared threshold, with zero silent repairs;
- recovery from sidecar crash, microphone removal, device change, model OOM, and lock/unlock;
- bounded audio/IPC queues during overload;
- hotkey and mouse/keyboard hooks survive long sessions and ignore self-injected input;
- clipboard contents are restored only when the app still owns the temporary clipboard state;
- no transcript/audio persisted by default and no network access during dictation;
- mixed-DPI and multi-monitor overlay tests pass, with deterministic fallback when anchoring
  confidence is low;
- one week of dogfooding produces a categorized failure log and regression cases, not only new
  training/tuning examples.

With these changes, the plan retains its strongest idea—measure the ASR choice locally—while
testing the defining Windows interaction early enough to influence the architecture rather
than merely validate it at the end.
