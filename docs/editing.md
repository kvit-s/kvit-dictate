# editing — the pill as a compose buffer

**Status: proposal. Nothing described here is implemented.** No code in `src/` does any of this
as of 2026-08-22 (commit `d53b6a0`). Every mechanism below is traced to the file that would have
to change, and everything I could not verify from the source is an open question in §15 rather
than an assertion.

Implements, and in two places **amends**, `plan.md`: §1.4 (the committed-prefix invariant — this
strengthens it), §1.5 (what the overlay renders), §1.6 (session lifetime), §1.7 (target safety),
and §1.8 (hotkey and stop-on-input — **this is the amendment**; stop-on-input stops meaning "end
the session" for the keys the pill claims). Read with `progress.md` §3c, which is the deferred
commit this feature is built on top of.

---

## 1. What it is, in one screen

Today a session's text accumulates invisibly and lands in the document when the session ends
(`Cadence::EndOfSession`, `progress.md` §3c). The overlay shows it but cannot touch it.

This proposal makes the pill an **editable compose buffer**. Nothing reaches the document until
the user says so.

| gesture | what happens |
|---|---|
| **speech** | appended to the end of the buffer, as now |
| **Enter** | the buffer is typed into the document; the session ends. **The Enter is consumed** — it is purely "commit", and is not replayed to the target |
| **Esc** | everything is discarded; the session ends. Unchanged from §1.8 |
| **hotkey again** | same as Enter. §1.8's toggle semantics are preserved exactly |
| **editing keys** (Backspace, Delete, arrows, Home/End, Shift+those, printable characters) | edit the buffer in place |
| **any other key** | commits, exactly as Enter does, and is **consumed** — it does not reach the target |
| **click inside the pill** | places the caret; drag selects |
| **click outside the pill** | commits and ends the session, as now |
| **focus moves to another window** | as now: the buffer goes to the clipboard with a tray warning (§3c) |

And one invariant on top:

> **After any edit, dictation only ever appends.** New speech is never inserted at the caret. It
> lands at the end of the buffer, and the caret moves there to meet it.

That rule is the user's, stated at the point the feature was proposed, and §3 explains why it is
also the only rule that can be implemented safely against a backend that revises its own tail.

## 2. Why — and the argument that matters is not the Enter bug

The feature started from a small annoyance: pressing Enter to end a line sends the Enter to the
application *first* and the dictated text lands after it, in the wrong order. Consuming Enter
fixes that, and §13's stage 1 is that fix alone.

The larger reason is that **compose-then-commit makes `plan.md` §1.4 trivially true.**

§1.4's invariant exists because there is no repair path: once bytes are in the user's document
they can never be taken back, so every rule in `commit_engine.h` is about never committing a byte
that a later hypothesis might contradict. `CommittedTextContradicted` is the violation that
"actually costs the user something", and `Result::salvage` exists because before it a contradicted
hypothesis silently lost the user the rest of the utterance.

Under compose-then-commit there is **exactly one injection per session, at Enter, of text the user
has read and approved.** The model can revise, contradict itself, hallucinate over silence, or
produce a Welsh sentence (`progress.md` §3d) — and all of it is visible in the pill, and all of it
is deletable, before a single byte reaches the document. The class of errors §1.4 spends its whole
design budget making rare becomes, for the document, *recoverable*.

That is worth more than the Enter fix, and it is the reason to accept the costs in §10.

## 3. The model: one buffer, two regions

The buffer already exists in two halves, and the overlay already renders them in two colours
(`OverlayStyle::color_pending` opaque and `color_unstable` at 60% alpha). This feature does not
introduce a new structure; it makes the first half addressable.

```
  App::deferred_                              CommitEngine::Result::pending_text
  |--------------------------------------|   |--------------------------------|
  settled — the user's, editable              unstable tail — the model's, not editable
                                    caret ^
```

* **The settled region** is `App::deferred_`: text the commit engine has finalised and handed over
  under `Cadence::EndOfSession`. It is complete, it is not going to change on its own, and it is
  the only region the caret can occupy.
* **The unstable tail** is the current utterance above the eligible prefix. It is still being
  revised on every `partial`. It is rendered, and it is **not addressable** — the caret clamps to
  `[0, len(settled)]`.

This is the whole freeze rule, and it needs no new "freeze" concept:

> **The user may edit only what the model has finished saying.** When an utterance finalises, its
> text joins the settled region and becomes editable at that moment.

The alternative — letting the user edit the tail and then suppressing revisions that would touch
it — was considered and rejected in §14. It requires a second, hand-maintained notion of "frozen"
alongside the one the commit engine already computes, and the two would drift.

### 3.1 The two authorities are allowed to disagree, and must

After an edit, `App::deferred_` and `CommitEngine::committed_text()` say different things. **That
is correct and nothing should reconcile them.**

* `CommitEngine` remains the authority on **what the model said**. Its violation checks compare an
  incoming hypothesis against its own record, never against the user's buffer, so a user edit can
  never manufacture a `CommittedTextContradicted`.
* `App::deferred_` becomes the authority on **what the user wants typed**.

The anchor estimator's source C (`plan.md` §1.5) reads `committed_text()` to know what change to
look for after an injection. Under `EndOfSession` nothing is injected until the session ends, so
source C has nothing to difference during a session either way; the divergence costs it nothing.

Anyone later tempted to "fix" the divergence by feeding edits back into the engine should read
§1.4's opening paragraph first: the engine's record is what makes the invariant checkable.

### 3.2 Speech onset moves the caret to the end

The user may be editing at offset 5 when a new utterance begins landing at offset 400. Two focus
points cannot both be followed by an 8-line window, and the failure mode of getting it wrong is
severe: **a Backspace against a caret the user cannot see.**

So on `LocalEvent::SpeechDetected` — once per utterance, not once per partial — the caret snaps to
the end of the settled region and the view scrolls to the tail. The user has just spoken; they
have moved on from wherever they were editing, and the alternative is a delete they did not watch.

The mic is **not** suspended by editing. A stray mutter appends a stray word, which is visible in
the pill and deletable — which is exactly the recoverability §2 is about, and is a better answer
than a mode toggle. The server-side VAD gate (`progress.md` §3b) already keeps most non-speech out.

### 3.3 Editing is grapheme-wise, not byte-wise

`util/utf8.h` already has everything required and it must be used: `next_grapheme` /
`prev_grapheme` for arrows and Backspace/Delete, `floor_grapheme` for clamping, and
`retract_to_word_boundary` for Ctrl+Backspace and Ctrl+arrow. A Backspace that removes a byte, or
a combining mark from its base letter, is a corruption the user cannot see until it reaches the
document.

## 4. Input: the keyboard

### 4.1 The pill never takes focus, so the hook is the only way in

`WS_EX_NOACTIVATE` is the flag `overlay.h` calls "the worst failure in the product if missing":
if the overlay takes focus the target loses it and injection lands in our own window. It also
takes the target out of the foreground, which makes `validate_target` fail, which is precisely the
path that sends a session's text to the clipboard instead of the document.

**So the pill cannot be given keyboard focus, ever, and no part of this feature may be implemented
by doing so.** Keystrokes reach the buffer the only other way they can: the `WH_KEYBOARD_LL` hook
that already exists, swallowing them by returning 1 without `CallNextHookEx`, and posting the
decision to the UI thread.

The consequence, stated plainly because it is the price of the feature:

> **While a session is composing, the keyboard belongs to the pill.**

### 4.2 What the hook may and may not do

`hotkey.h`'s governing rule is unchanged and absolute: the callback must never block, must not
allocate, and must not call into any other module. A slow hook is silently unhooked by Windows and
the hotkey dies with no error.

So the hook **classifies and posts**; it never edits. The split:

| runs in the hook callback (pure, O(1), allocation-free) | runs on the UI thread |
|---|---|
| decide the key's class: Commit / Cancel / Edit / Passthrough | apply the edit to the buffer |
| decide swallow or pass | move the caret, extend the selection |
| point-in-rect test against the cached pill rectangle | repaint |
| post one message | inject at Enter |

Classification belongs in `InputFilter`, which is already pure and already the only unit-tested
part of the module. `FilterOutcome` grows an edit command and a payload; nothing else about the
filter's shape changes.

### 4.3 The key classes

* **Commit** — Enter (unmodified), the hotkey chord, and *every non-modifier key that is not an
  editing key*. Swallowed, not replayed. Press it again and it reaches the application.
* **Cancel** — Esc. Unchanged from §1.8, including that it is **not** swallowed: `hotkey.cpp` gives
  the reason, and it still holds. Esc means "get me out of this" in the target too, and eating it
  would mean a dialog that refused to close while dictation happened to be running.
* **Edit** — Backspace, Delete, Left/Right/Up/Down, Home/End, any of those with Shift, Ctrl+A,
  Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Z, and printable characters. Swallowed.
* **Passthrough** — bare modifier down/up (Shift, Ctrl, Alt, Win). Never swallowed, never
  classified as Commit. A modifier alone expresses no intention, and swallowing a modifier's
  key-up is how "my Shift key is stuck on" bugs are born — `hotkey.cpp` already reasons this way
  for the binding key and the same reasoning applies.

**Ctrl+S, F5, Alt+Tab and friends fall in Commit.** They are swallowed, the text lands in the
target that is still focused, and the key does nothing else. This is better than today, where
Alt+Tab moves focus and the whole buffer goes to the clipboard — but it is a mode, and §15 has the
open question about the menu-bar wart that a passed-through Alt leaves behind.

`editing.capture_all_keys = false` (§12) restores today's behaviour for anyone who cannot live in
the mode: non-editing keys then pass through and end the session as they do now.

## 5. Input: the mouse

### 5.1 It is feasible, and `overlay.cpp` already anticipated it

`WS_EX_TRANSPARENT` routes clicks past the overlay to the target. It has to be dropped for the
pill to be clickable — and it can be, at runtime, with `SetWindowLongPtr(GWL_EXSTYLE)`, **only for
the duration of a composing session**.

What makes this safe rather than reckless is `WS_EX_NOACTIVATE`, which stays set. A click on a
NOACTIVATE window does not activate it, so the target stays foreground and `validate_target` keeps
passing. `overlay.cpp` already returns `MA_NOACTIVATE` from `WM_MOUSEACTIVATE`, with a comment
saying it is belt-and-braces "if a future change ever drops that flag". This is that change.

`WDA_EXCLUDEFROMCAPTURE` is untouched, so the anchor estimator is unaffected.

### 5.2 The mouse hook must stop eating its own clicks

`InputFilter::on_mouse_event` currently ends the session on **every** button-down. With a clickable
pill it must not do that for a click inside the pill's rectangle. The hook cannot ask the overlay
where it is — that would be a cross-module call from the callback — so the UI thread publishes the
pill rectangle into a plain seqlock-guarded slot, exactly as `write_click` / `read_click` already
publish the click point in the other direction, and the filter does a point-in-rect test.

* **inside the pill** — place the caret (button-down), extend the selection (move while down),
  end the selection (button-up). Not stop-on-input.
* **outside the pill** — commit and end the session. Unchanged from §1.8.
* **wheel over the pill** — scroll the pill (§7.3).
* **wheel elsewhere** — `click_invalidated`, unchanged.

### 5.3 Restoring transparency is the bug this will produce

A pill that stays non-transparent after a session eats clicks meant for the document, forever, and
the user's only recovery is to restart the process. That is the same shape as the
`note_session_ended()` bug in `progress.md` §8 — a state cleared on one of the two paths a session
can end through.

So the restore goes in `App::finish_dictation`, which is documented as **the one path every
session ends through**, and nowhere else. Not in `end_dictation`, which the abort paths skip.

## 6. What reaches the document

Unchanged in mechanism: `App::flush_deferred_commits` injects `deferred_` in one operation after
revalidating the target, and falls back to the clipboard when the target has gone. The only
difference is that `deferred_` has been edited.

The `plan.md` §1.4 invariant is untouched, and it is worth being explicit because "editing" sounds
like it violates it: **editing happens entirely before injection.** One injection, append-only, no
backspaces sent to any application, no repair path. The invariant is about the document, and the
pill is not the document.

### 6.1 This requires `Cadence::EndOfSession`

Under `Cadence::Immediate` the text is already in the document as it is spoken, and there is
nothing in the pill to edit. **The feature must be refused, and the setting disabled in the UI,
unless the cadence is `end-of-session`.** `Cadence::Immediate` is still the default in code
(`commit_engine.h`), so this is a live trap, not a hypothetical one — see §16.

### 6.2 What it does to ICER

`IcerCounters` measures committed graphemes that differ from the final hypothesis, per 1000
committed — the metric `plan.md` §1.4 chose to gate on because it counts text that is wrong *in
the actual document*.

Under compose-then-commit, nothing is irrevocably committed until the user approves it, so the
document-corruption rate this metric gates on goes to approximately zero by construction. ICER
does not become useless — it remains the right measure of **model** quality, and the bake-off in
`plan.md` §4 should keep reporting it — but it stops being a measure of what the user suffers.
Anyone reading an ICER number after this ships needs to know which of the two they are looking at.

## 7. Rendering

### 7.1 The caret does not blink

`overlay.h` is explicit that the overlay runs no render loop, because a compositor-rate loop for
text that changes a few times a second wastes power for no visible benefit. A blinking caret needs
a ~530 ms timer for the whole session and buys nothing but familiarity.

**A solid 1-DIP bar, positioned with `HitTestTextPosition`.** No timer, no power cost, and no
ambiguity about whether the caret is there.

### 7.2 Selection

A translucent rectangle behind the selected run, from `HitTestTextRange`, painted under the text.
Multi-line selections come back as several rectangles; all of them are painted.

### 7.3 Scrollback is a prerequisite, not a nicety

The pill shows the newest `kMaxLines` (8) with front-elision and **no scrollback** (`progress.md`
§3c), so a session-long buffer sits behind that window unseen. Editable-but-unreachable text is
not a feature; it is a trap. So:

* a scroll offset in lines, with the wheel driving it while the pointer is over the pill;
* **any caret movement scrolls the caret into view** — this is what makes arrow keys able to reach
  the top of a long buffer at all;
* the default position stays pinned to the bottom, so a session with no editing looks and behaves
  exactly as it does today;
* the pill's rectangle stays fixed, as §3c already made it, which is also what makes the hit-test
  in §5.2 stable.

## 8. Where the code changes

| file | change | size |
|---|---|---|
| **new** `src/edit_buffer.h/.cpp` | pure, Windows-free, no I/O: text + caret + selection + settled-region clamp, `apply(EditCommand)`. The module that carries the tests | medium |
| `src/hotkey.h/.cpp` | `InputFilter` gains key classification and the pill-rect hit test; `FilterOutcome` gains an edit command; a new `HotkeyPost` value; the fail-open heartbeat (§10.1) | medium |
| `src/overlay.h/.cpp` | caret and selection painting, scroll offset, runtime `WS_EX_TRANSPARENT` toggle, publish the pill rect | medium |
| `src/main.cpp` | route posted edit commands into the buffer; snap on `SpeechDetected`; commit on Enter; restore transparency in `finish_dictation` | small |
| `src/settings.h/.cpp` | the `editing` block (§12) | small |
| `src/session.h/.cpp` | **nothing.** The session machine does not learn about editing | none |
| `src/commit_engine.h/.cpp` | **nothing.** The engine keeps its own record and never sees an edit | none |
| `src/injector.h/.cpp` | **nothing.** One injection of a string, as now | none |

Two of those rows are the point: the state machine and the commit engine are the modules whose
correctness `plan.md` §7 gates on, and this feature does not touch either.

## 9. Session lifetime

Unchanged, with one addition. `plan.md` §1.6's two timers still run; the session-silence timer
still ends a session that has heard nothing for 120 s.

**Open, and it matters:** should the session-silence timer be paused while the user is editing?
Editing is not silence in the sense §1.6 means — the user is present and working — and having a
buffer commit itself mid-edit because the timer expired would be a bad surprise. My answer is yes,
pause it on the first edit key and leave it paused until the next `SpeechDetected`. The endpoint
timer is unaffected: it ends utterances, not sessions. Recorded as a question in §15 because it is
a change to §1.6 that the user has not been asked about.

## 10. Safety, and the one failure mode that is worse than the feature

### 10.1 A wedged UI thread with the keyboard captured

The hook runs on its own thread and swallows keys **independently of the UI thread**. If the UI
thread hangs while a session is composing, keys are swallowed by a thread that is still healthy and
delivered to a thread that will never process them. The user's keyboard is dead, in every
application, with no way out short of `Ctrl+Alt+Del`.

This is strictly worse than any failure the product can produce today, and it is not hypothetical:
the existing hook watchdog reinstalls a hook that Windows removed, which does not help here — the
hook is fine, the consumer is not.

**The mitigation is mandatory and is the single most important property of the feature: fail open.**

* The UI thread stamps a monotonic heartbeat every message-loop iteration.
* Before swallowing anything on behalf of the pill, the hook compares the heartbeat against now.
  If it is more than **2000 ms** stale, the hook stops swallowing and stops posting, permanently,
  for that session. Keys go to the application. The user has their machine back.
* The comparison is a load and a subtraction. It costs nothing against the callback budget that
  `Stats::max_callback_us` exists to watch.

Additionally, **Esc clears capture inside the filter itself**, synchronously in the hook callback,
rather than waiting for the UI thread to confirm via `note_session_ended()`. Esc is the user saying
"get me out of this" and it must not depend on the thread that might be the problem.

### 10.2 It looks more like a keylogger

`plan.md` §6 already lists heuristic antivirus detection as a risk for the existing hook. Swallowing
most of the keyboard for seconds at a time is a stronger signal. Nothing to be done about it here
beyond knowing it, and not being surprised when it happens.

### 10.3 The clipboard rescue path becomes likely

`progress.md` §6 item 4 lists the `set_clipboard_text` rescue for an orphaned deferred buffer as one
of two clipboard paths that **have never executed**. This feature makes it much more likely to run —
a user editing in the pill is a user who might Alt+Tab away mid-edit — and it will be carrying more
text, and text they have personally worked on, when it does. It needs a test before this ships,
not after.

## 11. Testing

The shape the codebase already uses: **put the logic somewhere pure and test it there.**

* `EditBuffer` — no Windows, no I/O, no clock. Caret clamped to the settled region; the tail not
  addressable; Backspace and arrows moving by grapheme cluster and not by byte; Ctrl+Backspace to
  the word boundary; selection replacement; the snap on speech onset; appending while a selection
  is active; an empty buffer; a buffer that is entirely selected then typed over.
* `InputFilter` — key classification is a pure function of (vk, modifiers, composing). Test the
  full class table, that bare modifiers are never Commit, that Esc still passes through, and the
  point-in-rect branch for the mouse.
* The fail-open heartbeat — testable at the filter level by injecting a stale stamp, without a
  hung thread.
* `layout_overlay` and the scroll arithmetic — already the pattern for pure layout helpers.

`src/main.cpp` has no tests and this feature must not put anything interesting there. Anything that
would need a test in `main.cpp` belongs in `EditBuffer` instead.

## 12. Settings

```json
"editing": {
  "enabled": true,
  "mouse": true,
  "capture_all_keys": true,
  "commit_key": "enter",
  "pause_session_timer_while_editing": true
}
```

`enabled` is forced off when `stability.cadence` is not `end-of-session` (§6.1), and the settings
dialog must say why rather than silently ignoring it.

`settings.cpp` has **no tests at all** (`progress.md` §6 item 5) and is 2409 lines. This block
should not be the fifth thing added to it untested; adding it is a good occasion to write the
first round-trip tests that file has ever had.

## 13. Staging

Each stage is independently shippable and independently useful, and each one ends at a commit.

1. **Enter commits, and is consumed.** No editing, no buffer, no mouse. Non-editing keys commit and
   are consumed. This is the fix for the annoyance that started the feature, and it is perhaps
   fifty lines across `hotkey.cpp` and `main.cpp`.
2. **Keyboard editing.** `EditBuffer`, caret and selection painting, scrollback, key classification,
   the fail-open heartbeat. This is the bulk of the work.
3. **Mouse.** The transparency toggle, the pill-rect hit test in the mouse hook, click-to-place,
   drag-select, wheel-scroll.
4. **Optional.** Ctrl+Z undo over a bounded snapshot stack; Ctrl+C / Ctrl+X / Ctrl+V within the
   pill; word-wise motion.

Do not start stage 2 before stage 1 has been used for a real day of dictation. §2's argument is
strong but it is an argument, and the mode in §4.1 is the kind of thing that reads well and feels
wrong.

## 14. Alternatives considered and rejected

* **Give the pill real focus and let Windows do the editing.** Rejected outright: `WS_EX_NOACTIVATE`
  is what keeps the target foreground, and losing the target is the failure that sends the whole
  buffer to the clipboard. This is not a trade-off, it is the flag `overlay.h` names as the worst
  failure in the product.
* **Let the user edit the unstable tail, suppressing revisions that would touch it.** Requires a
  second, hand-maintained notion of "frozen" beside the one `CommitEngine` already computes; they
  would drift, and the drift would present as the model's revisions being silently dropped. §3's
  rule gets the same result from structure that already exists.
* **Suspend the microphone on the first edit key, resume on the hotkey.** A cleaner-sounding mode,
  but it needs a resume gesture that collides with the hotkey's §1.8 meaning, and it trades a
  visible-and-deletable stray word for an invisible failure to record what the user said.
* **Replay the committed key to the target after the text lands.** One Enter would then both commit
  and send, which is nicer in a chat box — and occasionally sends a half-finished message. The user
  chose consumed; safety agrees.
* **Snapping the view but not the caret on speech onset.** Two focus points and one 8-line window;
  the loser is a Backspace against a caret nobody can see.
* **A blinking caret.** A timer for the whole session against `overlay.h`'s explicit no-render-loop
  position, to gain nothing but familiarity.

## 15. Open questions

None of these should be answered by guessing.

1. **The passed-through Alt.** Alt down reaches the shell, Tab is swallowed, Alt up reaches the
   shell — does the target show its menu bar? Same family as the Start-menu problem `plan.md` §1.8
   already hacks around with `suppress_start_menu()`, and probably the same answer, but it must be
   observed rather than assumed.
2. **Does `SetWindowLongPtr(GWL_EXSTYLE)` toggling `WS_EX_TRANSPARENT` take effect without a
   `SetWindowPos(SWP_FRAMECHANGED)`?** Documented to need the latter for some styles. Untested here.
3. **Does a click on a `WS_EX_NOACTIVATE` topmost layered window ever disturb the foreground
   window**, on any of the targets in `docs/supported-targets.md`? The theory says no; the theory
   also said source C's confidence score would separate two cases, and §5 of `progress.md` records
   how that went.
4. **Should the session-silence timer pause during editing?** §9. My answer is yes; it amends
   `plan.md` §1.6 and the user has not been asked.
5. **How long is too long?** There is no cap on the buffer. A twenty-minute session produces a
   buffer nobody wants to inject in one `SendInput` burst at ~1–2 ms/char (`plan.md` §1.7).
   The clipboard path is the obvious answer above some size, but the size is unmeasured.
6. **Does the mode survive a noisy room?** `progress.md` §4b says the noisy-room case is the one it
   does not cover, and a stray word appearing in an editable pill is only self-correcting if the
   user notices it.

## 16. The check that `progress.md` §8 exists to force

Six sessions running, the recurring failure is **a contract implemented on one side and never
called from the other**, and a green test suite answers a different question. This feature has an
unusually good supply of ways to reproduce it. Before believing any part of it works:

* **`EditBuffer` will be fully tested and reachable from nowhere** if `InputFilter` never classifies
  a key as Edit, or if the post is added to a `HotkeyPost` case nobody handles. That is exactly the
  `o.error` bug: a reader added to a path the failure does not take.
* **`editing.enabled` defaults to true and `Cadence::Immediate` is still the default in code.** A
  user who has not set `end-of-session` gets a feature that parses, validates, round-trips through
  `settings.json` and does nothing — which is `min_confidence` and `is_usable()` verbatim.
* **The mouse hook's pill-rect test is the `last_voice_ms` shape**: a value the tests supply and the
  product may not. If the UI thread never publishes the rect, the slot reads zero, every click is
  outside the pill, and the mouse silently reverts to ending the session — with a green suite,
  because the tests write the rect themselves.
* **The transparency restore must be on the path every session ends through**, not on the one the
  gesture takes. See §5.3.

The habit: before trusting that something works, check which entry points the product actually
calls.
