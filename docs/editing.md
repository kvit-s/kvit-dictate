# editing — the pill as a compose buffer

**Status: implemented, stages 1–3, and unproven in use.** Everything below is in `src/` as of
2026-08-22 — the document has been left in the future tense it was written in, because it is the
argument for the design rather than a description of the code, and rewriting it into the past
tense would lose the reasons. What changed is this header and §17.

The three commits are `Enter finishes the session…` (stage 1), `The pill becomes a compose
buffer…` (stage 2) and `The pill becomes clickable…` (stage 3). Stage 4's contents arrived inside
stage 2 rather than after it: §4.3 puts Ctrl+Z, Ctrl+C/X/V and word-wise motion in the Edit class,
and a key that is swallowed and then does nothing is worse than the feature it was deferring.

**§13's staging gate was not honoured, and that is a real deviation.** "Do not start stage 2
before stage 1 has been used for a real day of dictation" — stages 1 to 3 were written in one
sitting. The argument for the gate stands: §4.1's mode is the kind of thing that reads well and
feels wrong, and nothing here has been used in anger. §17 lists what a day of use is expected to
answer.

Implements `plan.md` §1.4 (the committed-prefix invariant — this strengthens it), §1.5 (what the
overlay renders), §1.7 (target safety) and §1.9 (the microphone privacy promise, which §3.2 leans
on hard). It **amends** two sections, and both amendments are stated where they happen:

* **§1.8** — twice. Stop-on-input stops meaning "end the session" for the keys the pill claims
  (§4.3–4.4); and **the toggle is gone** — the hotkey only ever starts or resumes dictation, never
  finalises it, so finishing is always Enter (§4.5).
* **§1.6** — the session-silence timer measures silence *while listening*, not wall-clock since
  activation, because editing closes the microphone (§9).

Read with `progress.md` §3c, which is the deferred commit this feature is built on top of.

---

## 1. What it is, in one screen

Today a session's text accumulates invisibly and lands in the document when the session ends
(`Cadence::EndOfSession`, `progress.md` §3c). The overlay shows it but cannot touch it.

This proposal makes the pill an **editable compose buffer**. Nothing reaches the document until
the user says so.

A session is in one of two states, and the pill and the tray always say which:

* **Dictating** — the microphone is open, speech is appended to the end of the buffer.
* **Editing** — the microphone is **closed**, and the buffer is a text field.

**The hotkey only ever means "dictate".** It starts a session and it resumes one; it never ends
one and never commits anything. Exactly four things dismiss the pill — Enter (commit), Esc
(discard), a click outside it, and focus leaving the target window — and every one of them is
either an explicit decision or the user physically going somewhere else. §4.5 is why this is worth
giving up `plan.md` §1.8's toggle semantics for.

| gesture | dictating | editing |
|---|---|---|
| **speech** | appended to the end of the buffer, as now | the mic is closed; nothing is heard |
| **any editing key** (Backspace, Delete, arrows, Home/End, Shift+those, printable characters) | **closes the microphone** and switches to editing, then applies the key | edits the buffer in place |
| **hotkey** | nothing — you are already dictating | **resumes dictating**: reopens the mic, drops the selection, caret to the end |
| **Enter** | the buffer is typed into the document; the session ends. **The Enter is consumed** — it is purely "commit", never replayed to the target | the same |
| **Esc** | everything is discarded; the session ends. Unchanged from §1.8 | the same |
| **volume, media, brightness, PrtSc, the lock keys** | pass through untouched — none of them types into the target | the same |
| **any other key** (Ctrl+S, F5, Alt+Tab, Win+E…) | **nothing.** Swallowed and ignored, with a one-line hint in the pill. It does not commit and it does not reach the target | the same |
| **click inside the pill** | places the caret, and so closes the microphone | places the caret; drag selects |
| **click outside the pill** | commits and ends the session, as now | the same |
| **focus moves to another window** | as now: the buffer goes to the clipboard with a tray warning (§3c) | the same |

And one invariant on top:

> **After any edit, dictation only ever appends.** New speech is never inserted at the caret. It
> lands at the end of the buffer, and the caret moves there to meet it — on the user's own resume
> gesture, never on a microphone the user had stopped watching.

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

### 3.2 Editing closes the microphone

**The first edit key stops the capture.** Not gates it, not discards its frames — stops it, at the
device. Speech and editing are disjoint states and the user is only ever in one of them.

An earlier revision of this document kept the mic open through editing and let a stray word land
in the buffer, on the argument that a visible stray word is better than an invisible failure to
record. That argument was wrong twice over:

* **The stray word is not merely visible, it is disruptive.** That revision also snapped the caret
  to the end of the buffer on speech onset, so a mutter or a colleague's question while the user
  was concentrating on a correction would *move the caret out from under them mid-edit*. The two
  rules combined to make the failure worse than either alone.
* **"Keep capturing and discard the frames" is not available**, and this is not a judgement call.
  `audio_capture.h` states it: "the mic is open only between `start()` and `stop()` — `plan.md`
  §1.9 makes that a privacy promise, and the tray state is driven from it, so nothing else may
  hold the device open." A pill that says *paused* over a lit microphone light breaks the one
  promise the product makes about the microphone.

So the state is real: capture stops, the tray and the pill say so, and the mic light goes out.

### 3.3 Suspending mid-utterance, and resuming

**If an utterance is open when the edit key arrives**, it is flushed rather than abandoned:
`Effect::FlushPendingAudio` (which already exists, for `stop`) pushes what has been captured, the
sidecar finishes decoding it, and the resulting `final` joins the settled region. The user pressed
an arrow key to go and fix something they just said; the thing they just said had better be
editable by the time they get there. No protocol change — this is the endpoint path, reached by a
gesture instead of by a timer.

**Resuming is an explicit gesture**, because a closed microphone cannot notice that you want to
talk to it:

* **the hotkey** — which means "dictate" here as it does everywhere else (§4.5), so there is
  nothing state-dependent to remember;
* **a microphone button on the pill**, once the mouse exists (stage 3, §13).

On resume the capture restarts, a `discontinuity` is emitted for the gap (the existing
`on_capture_glitch(gap_ms, reason)` path, `reason = "editing"` — a stateful backend must not be
told nothing happened), **the selection is dropped, and the caret moves to the end of the buffer.**

That last part is what §1's invariant asked for, and note what has changed about it: the caret
moves because the user made a gesture that means "I am going to talk now", not because a
microphone surprised them. It is a consequence of an explicit action rather than an interruption,
which is the whole difference between this design and the one it replaces.

### 3.4 Editing is grapheme-wise, not byte-wise

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

* **Commit** — Enter (unmodified). **Only Enter.** Swallowed, not replayed: press it again and it
  reaches the application.
* **Dictate** — the hotkey chord. Starts a session from idle, resumes one from editing (§3.3), and
  does nothing at all while already dictating. It never commits and never cancels.
* **Cancel** — Esc. Unchanged from §1.8, including that it is **not** swallowed: `hotkey.cpp` gives
  the reason, and it still holds. Esc means "get me out of this" in the target too, and eating it
  would mean a dialog that refused to close while dictation happened to be running.
* **Edit** — Backspace, Delete, Left/Right/Up/Down, Home/End, any of those with Shift, Ctrl+A,
  Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Z, and printable characters. Swallowed.
* **Passthrough** — two disjoint groups, both never swallowed and never committing.
  * bare modifier down/up (Shift, Ctrl, Alt, Win). A modifier alone expresses no intention, and
    swallowing a modifier's key-up is how "my Shift key is stuck on" bugs are born — `hotkey.cpp`
    already reasons this way for the binding key and the same reasoning applies.
  * the keys that change the machine rather than the document: volume, media transport, brightness,
    Print Screen, Caps/Num/Scroll Lock (unless one of them is the binding). None of them can put a
    character anywhere, so swallowing them is pure loss. Turning the volume down mid-dictation
    should simply work.
* **Ignore** — everything else. Ctrl+S, F5, Alt+Tab, Win+E, the function keys. Swallowed, and
  **nothing happens**.

### 4.4 Why "everything else" does nothing, rather than committing

An earlier revision of this document had the Ignore class commit, exactly as Enter does. That rule
came from the feature's first sketch, when the pill was a passive preview and any keystroke
honestly meant "the user has moved on to something else" — `plan.md` §1.8's stop-on-input, which is
right for a preview.

**It stops being right the moment the pill is something the user is operating.** Someone editing a
paragraph in the pill presses Ctrl+S from habit and their half-finished text is typed into the
document and the session is over — an irreversible action, triggered by a key that means something
else, with no warning. The rule outlived the premise that justified it; `progress.md` §8 is a
catalogue of that exact failure and this is one more instance of it.

So the only things that end a composing session are the things that *mean* it: Enter, the hotkey,
Esc, a click outside the pill, focus moving to another window, and the session-silence timer. That
list is long enough. No stray key needs to be on it.

The cost is that Ctrl+S silently does nothing, which is its own small confusion — so it is not
silent. The pill shows a single line naming the key and saying what to do about it:

> `Ctrl+S — press Enter to finish first`

It expires after a couple of seconds on the tick `App` already runs, needs no new timer, and is not
`flag_violation` — a violation is a persistent mark on text that may be wrong (`plan.md` §1.4), and
this is neither persistent nor about the text.

`editing.on_foreign_key = "end-session"` (§12) restores today's behaviour for anyone who wants it:
the key then passes through and ends the session, as it does now.

### 4.5 The hotkey means exactly one thing, and that costs §1.8's toggle

An earlier revision had the hotkey commit while dictating and resume while editing — one key, two
meanings, chosen by state — and spent three paragraphs defending it. The defence was sound and the
design was still worse than this one, which is: **the hotkey means "dictate", and nothing else,
ever.**

What it buys:

* **No destructive gesture is state-dependent.** §4.4 objects to Ctrl+S committing because it
  fires an irreversible action from a key that means something else; a hotkey that commits in one
  state and resumes in another is a milder case of the same complaint, and the reply to it was
  "the mis-press fails in the safe direction" — which is a good answer to a question better not
  asked.
* **Every dismissal is now an explicit decision or a physical departure.** Enter, Esc, a click
  outside, focus leaving. There is no key that ends a session as a side effect of meaning
  something else.
* **The mode is describable in one sentence**, which §4.1's keyboard capture badly needs: *Win+Y
  talks, Enter finishes, Esc throws it away.*

What it costs, and it is a real cost:

* **`plan.md` §1.8's toggle semantics are gone.** "Press to start; stops on … the hotkey again" was
  chosen to match the `Win+H` gesture the product replaces, and this abandons that half of the
  match. Finishing is now always Enter. The muscle memory is a genuine loss and it is the reason
  this is the second amendment §1.8 takes from this document.
* **The hotkey does nothing while dictating**, which is a key that has to be explained by its
  absence. The obvious alternative — hotkey pauses the microphone, symmetrically with resuming it
  — is a one-line change if the no-op turns out to feel dead in use. It is not the default because
  "always start/continue" is a rule with no exceptions, and "toggle the mic" reintroduces one.

Once the mouse exists (stage 3, §13) the pill also carries a microphone button, which says what it
does without anyone having to remember a key.

### 4.6 The target never loses focus — including while editing

Worth stating on its own, because the natural assumption is the opposite: *while you are editing in
the pill, is the target window still focused?*

**Yes. Throughout. The pill is never focused, not for a moment.** `WS_EX_NOACTIVATE` is what
guarantees it, `WM_MOUSEACTIVATE` returning `MA_NOACTIVATE` is the second line of the same defence
(§5.1), and the window is shown with `SW_SHOWNOACTIVATE` rather than `SW_SHOW` for the same reason.

The consequences are all the good kind:

* **There is nothing to restore at commit time.** `SendInput` types into whatever is focused, and
  what is focused is — still, unchanged, the whole time — the window the user pressed the hotkey
  in. No `SetForegroundWindow`, no focus juggling, no dismiss-then-refocus dance. §6 injects and
  the text lands.
* **`validate_target` keeps passing**, because its requirement is that the captured window is still
  the foreground one. The pill cannot violate that; it is not in the running.
* **Focus moving away is still a real event** — the user clicked another window, or something stole
  it — and it still ends the session with the buffer going to the clipboard (§10.3). What changed
  is that *editing* is no longer one of the ways it can happen.

The cost is the one §4.1 already stated, and this is its other half: because the pill is never
focused, the only way it can receive a keystroke is to take it from the window that *is* focused.
There is no third option. Either the overlay takes focus — and the target is lost, which is the
worst failure `overlay.h` names — or the hook steals keys, and the user is in a mode. This design
steals keys.

One visible artefact falls out of it: **the target's own caret keeps blinking** while the user
edits in the pill, so there are two carets on screen. In `FloatingPill` mode they are far enough
apart to read as what they are. In `Inline` mode the pill sits in the target's text flow and they
would be adjacent, which is one more reason inline is not the mode this feature is designed for
(`progress.md` §5 has the others).

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
  end the selection (button-up). Not stop-on-input. Placing a caret is an edit gesture, so it
  closes the microphone exactly as the first edit key does (§3.2) — the mouse must not open a
  second route into editing mode that skips the suspend.
* **outside the pill** — commit and end the session. Unchanged from §1.8.
* **wheel over the pill** — scroll the pill (§7.3). Scrolling is **not** an edit gesture and does
  not close the microphone: `hotkey.cpp` already reasons that scrolling to re-read what you
  dictated is a normal thing to do mid-session, and it stays true here.
* **wheel elsewhere** — `click_invalidated`, unchanged.

### 5.3 Restoring transparency is the bug this will produce

A pill that stays non-transparent after a session eats clicks meant for the document, forever, and
the user's only recovery is to restart the process. That is the same shape as the
`note_session_ended()` bug in `progress.md` §8 — a state cleared on one of the two paths a session
can end through.

So the restore goes in `App::finish_dictation`, which is documented as **the one path every
session ends through**, and nowhere else. Not in `end_dictation`, which the abort paths skip.

### 5.4 The pointer shape is the only affordance the pill has

The pill takes no focus, has no border a mouse can grab and no control that looks like a control
apart from the microphone button. Nothing about it says "you may click here" — so the pointer has
to, and it is the one channel that costs nothing and is read without being taught.

`overlay.cpp` answers `WM_SETCURSOR`: an **I-beam** over the pill, because the pill *is* a text
buffer and a click places the caret, and a **hand** over the microphone button, because a click
there acts. The message only ever arrives while `WS_EX_TRANSPARENT` is dropped — while
click-through, the window underneath answers it and the pointer is none of our business — so the
shape is true by construction: it changes exactly when, and only when, a click would land here.

The window class cursor is an arrow rather than `nullptr`. A null class cursor leaves the pointer
wearing whatever shape it arrived in, which is how a busy cursor from another window ends up
sitting over a pill that is not busy.

A drawn microphone cursor was considered and not built. It would need a resource the client does
not otherwise have, and it would say *what the window is* — which the pill already says, in
words — where the I-beam says *what the click will do*, which is the thing that was missing.

## 6. What reaches the document

Unchanged in mechanism: `App::flush_deferred_commits` injects `deferred_` in one operation after
revalidating the target, and falls back to the clipboard when the target has gone. The only
difference is that `deferred_` has been edited.

**In particular there is no focus restoration step, because focus never went anywhere** — §4.6 has
the argument. Nothing in this feature may add one: `injector.cpp` never restores focus precisely
because `SendInput` types into whatever *is* focused, and a design that moves focus away and back
would be racing the user for it.

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

### 7.4 The pill says which state it is in, and this is load-bearing

`OverlayState` gains **`Editing`**, and it must reach the tray as well as the pill.

This is not decoration. Three separate rules in this document rest on the user being able to see
whether the microphone is open: §3.2's privacy promise (a closed device must look closed), §3.3's
resume (a gesture nobody makes if they cannot tell it is needed), and §4.5's no-op hotkey — a key
that does nothing while dictating is only tolerable if the user can see *why* it did nothing.
`plan.md` §1.6 already requires listening state to be unambiguous in the tray for its own reasons;
this feature gives that requirement three more jobs.

A microphone glyph — live, and struck through — plus the state word. The glyph is what the user
checks against the hardware light on their machine, so the two must never disagree.

### 7.5 The pill names the keys, because nothing else can

§4.1 is what forces this: the pill can never take focus, so it has no menu, no tooltip and no
window a user can right-click. Every gesture in §4.3 has to be learned from somewhere, and the
only surface that is on screen at the moment it matters is the pill itself.

So the bottom line of the pill is a standing legend of key names, in the dimmest colour the style
defines, composed from the state being painted:

| state | line |
|---|---|
| Listening / Speaking, composing | `Enter to finish · Esc to cancel · type or click to edit` |
| Editing | `Enter to finish · Esc to cancel · Win+Y to resume` |
| Listening / Speaking, not composing | `Win+Y to finish · Esc to cancel · any key ends it` |
| anything else | nothing |

Three rules make it honest rather than decorative, and `key_legend` in `overlay.h` is a pure
function with tests for exactly that reason:

* **It never names a key that does nothing in the state it names it in.** The hotkey appears only
  while editing, because §4.5 makes it a deliberate no-op while the microphone is already open.
* **It never names Enter as a commit outside a composing session.** With `editing.enabled` false —
  or any cadence but end-of-session (§6.1) — Enter is an ordinary keystroke that passes through to
  the target and ends the session, so the line says what `plan.md` §1.8 actually does instead. This
  is the one that would be actively harmful to get wrong: it would teach the user that a
  destructive key is a safe one.
* **It is gone in every state where the session is not the user's to steer**, `Finalizing`
  included: by then `stop` has gone out and none of the three keys still means what it meant.

It is drawn in the floating pill only. `StatusOnly` has nothing to steer, and the inline overlay is
a label attached to a caret whose whole job is to stay out of the way — a standing legend there
would make it the largest thing on the line it annotates. The pill is the mode with the room, and
it is where the compose buffer, the mouse (§5) and the microphone button already live.

There is no setting to hide it. If a day of use says an expert wants it gone, the honest form is a
setting and not a fade-after-N-sessions timer: something that disappears on a schedule is exactly
the thing a user cannot find again when they need it.

## 8. Where the code changes

| file | change | size |
|---|---|---|
| **new** `src/edit_buffer.h/.cpp` | pure, Windows-free, no I/O: text + caret + selection + settled-region clamp, `apply(EditCommand)`. The module that carries the tests | medium |
| `src/hotkey.h/.cpp` | `InputFilter` gains key classification and the pill-rect hit test; `FilterOutcome` gains an edit command; a new `HotkeyPost` value; the fail-open heartbeat (§10.1) | medium |
| `src/overlay.h/.cpp` | caret and selection painting, scroll offset, runtime `WS_EX_TRANSPARENT` toggle, publish the pill rect | medium |
| `src/main.cpp` | route posted edit commands into the buffer; stop and restart the capture on suspend/resume; caret to the end on resume; commit on Enter; restore transparency in `finish_dictation` | small |
| `src/settings.h/.cpp` | the `editing` block (§12) | small |
| `src/session.h/.cpp` | two `LocalEvent` values (`EditingBegan` / `DictationResumed`) that pause and re-arm the session-silence timer and flush a mid-utterance suspend (§3.3, §9). No new state, no change to any existing verdict. Plus one rename — see below | small |
| `src/commit_engine.h/.cpp` | **nothing.** The engine keeps its own record and never sees an edit | none |
| `src/injector.h/.cpp` | **nothing.** One injection of a string, as now | none |
| `src/audio_capture.h/.cpp` | **nothing.** `start()` and `stop()` already do what suspend and resume need, and `stop()` is already documented as safe when not started | none |

The last three rows are the point: the commit engine and the injector are where `plan.md` §1.4's
invariant lives, and this feature does not touch either. The session machine's change is the
smallest thing that can express "the user is present but not speaking", and it is confined to
timer arming — no verdict, no message, no protocol state.

**The rename is not cosmetic.** `LocalEvent::HotkeyPressed` is the finalise event — "from Running
it produces `FlushPendingAudio` + `stop`" — and after §4.5 the hotkey no longer does that. Enter
does. Leaving the name alone would put an enumerator called `HotkeyPressed` on a path the hotkey
never takes, which is the single most reliable way to make the next reader wrong about what the
product does. It becomes **`FinalisePressed`**, and the hotkey's own arrival maps to
`DictationResumed` or to nothing at all.

`to_string(LocalEvent)` spells it `"hotkey_pressed"` and `session.h` warns that the fixture runner
compares these as strings — but **no fixture under `tests/fixtures/` uses this one** (checked
2026-08-22: zero files match). So the rename is confined to `src/session.{h,cpp}`,
`tests/test_session.cpp` and `tests/test_fixtures.cpp`, and the cross-language contract is not
involved. Confirm that is still true before doing it rather than trusting this paragraph.

## 9. Session lifetime

`plan.md` §1.6's two timers still run while dictating, unchanged.

**The session-silence timer is paused while editing, and this is now a decision rather than a
question.** It was open while editing merely coexisted with an open microphone; once editing
*closes* the microphone it stops being a judgement call. That timer measures how long a session
survives **with no speech**, and during editing there is no microphone to be silent into. Leaving
it armed would mean a session ending itself — typing a half-finished buffer into the document —
because the user spent 120 s reading what they had dictated. It is armed again on resume.

The endpoint timer is unaffected: it ends utterances, not sessions, and there is no open utterance
to end (§3.3 flushes one that was in progress).

This does amend `plan.md` §1.6, and the amendment is small and worth stating in one line: the
session-silence timer measures **silence while listening**, not wall-clock since activation.
`progress.md` §8 records what happened the last time those two were confused — a hard two-minute
cap on every dictation, with a green test suite.

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
  the word boundary; selection replacement; the caret move and dropped selection on resume;
  appending while a selection is active; an empty buffer; a buffer that is entirely selected then
  typed over.
* `InputFilter` — key classification is a pure function of (vk, modifiers, composing). Test the
  full class table, that bare modifiers are never Commit, that Esc still passes through, and the
  point-in-rect branch for the mouse.
* The fail-open heartbeat — testable at the filter level by injecting a stale stamp, without a
  hung thread.
* `SessionMachine` — the two new events against a `ManualClock`: that `EditingBegan` disarms the
  session-silence timer, that `DictationResumed` re-arms it, and — the one that will actually
  break — that a session which spends longer than `session_silence_ms` in editing **does not end**.
  That last assertion is the whole point of §9 and it is the one a careless implementation passes
  by accident and a correct one passes on purpose.
* `layout_overlay` and the scroll arithmetic — already the pattern for pure layout helpers.

`src/main.cpp` has no tests and this feature must not put anything interesting there. Anything that
would need a test in `main.cpp` belongs in `EditBuffer` instead.

## 12. Settings

```json
"editing": {
  "enabled": true,
  "mouse": true,
  "on_foreign_key": "ignore",
  "commit_key": "enter"
}
```

`enabled` is forced off when `stability.cadence` is not `end-of-session` (§6.1), and the settings
dialog must say why rather than silently ignoring it.

`on_foreign_key` is `"ignore"` (§4.4, the default) or `"end-session"` (today's behaviour: the key
passes through to the target and stop-on-input ends the session). It is deliberately an enum and
not a boolean: the two values name two different things that happen, and a `capture_all_keys: true`
would have left the reader to guess which.

**There is deliberately no knob for closing the microphone during editing**, and none for pausing
the session-silence timer. An earlier draft had the second one. Both are consequences of §3.2
rather than preferences: a mic that stays open contradicts `audio_capture.h`'s privacy promise, and
a session timer that stays armed with no microphone open is measuring nothing and will end a
session mid-edit. Neither has a defensible "off" position, and a setting whose wrong value is
indefensible is a bug with a UI.

`settings.cpp` has **no tests at all** (`progress.md` §6 item 5) and is 2409 lines. This block
should not be the fifth thing added to it untested; adding it is a good occasion to write the
first round-trip tests that file has ever had.

## 13. Staging

Each stage is independently shippable and independently useful, and each one ends at a commit.

1. **Enter commits, and is consumed.** Enter alone: every other key, **including the hotkey**,
   keeps today's behaviour exactly. No editing, no buffer, no mouse, and **no mode**. This is the
   fix for the annoyance that started the feature and it is perhaps fifty lines across
   `hotkey.cpp` and `main.cpp`.

   Two things deliberately do **not** arrive here, for the same reason. The Ignore class of §4.4
   is justified by the user operating the pill, and in stage 1 they are not — swallowing Ctrl+S to
   protect a buffer nobody can edit is a mode with no feature behind it. And §4.5's hotkey change
   is justified by there being an editing state for it to resume *into*; land it in stage 1 and it
   only subtracts, leaving a key that used to finish a session and now does nothing.
2. **Keyboard editing.** `EditBuffer`, caret and selection painting, scrollback, key classification
   including §4.4's Ignore class and its hint line, the microphone suspend and resume of §3.2–3.3
   with `OverlayState::Editing` reaching both pill and tray, §4.5's hotkey change and the
   `FinalisePressed` rename that goes with it, the paused session timer, and the fail-open
   heartbeat. This is the bulk of the work, and it is the stage that introduces the mode.

   **Measure the resume latency first** (§15 question 4). It is the one thing in this stage that
   could change its design rather than its code, and it costs an afternoon to find out.
3. **Mouse.** The transparency toggle, the pill-rect hit test in the mouse hook, click-to-place,
   drag-select, wheel-scroll, and the microphone button — the one control that says what it does
   without anyone having to remember a key, and the answer for whoever finds §4.5's no-op hotkey
   more confusing than the toggle it replaced.
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
* **Keep the microphone open through editing, and let stray speech land in the buffer as a
  visible-and-deletable word.** This document said so until it was read back; §3.2 has the
  retraction. It was wrong on the product (the caret-snap it required moves the caret out from
  under a user mid-edit) and unavailable on the facts (`audio_capture.h` makes `start()`/`stop()`
  the privacy promise, so a discarded feed would be a lit microphone light over a pill claiming to
  be paused).
* **A dedicated resume chord — Ctrl+Space, Ctrl+Enter.** Unnecessary once the hotkey means only
  one thing, and Ctrl+Enter in particular sits one slip away from the irreversible key.
* **The hotkey commits while dictating and resumes while editing.** Two revisions of this document
  said so, the second with a three-paragraph defence; §4.5 has the retraction. The defence was
  sound and the design was still worse, because it made a destructive gesture state-dependent —
  the same objection §4.4 raises against Ctrl+S, applied more gently to a key the user presses far
  more often.
* **Keeping `plan.md` §1.8's toggle.** It is the `Win+H` gesture this product set out to match, and
  giving it up is the largest single behaviour change in this document. Kept losing to the rule
  that no key should end a session as a side effect of meaning something else — and the toggle's
  second press means exactly that once there is a buffer worth protecting.
* **Replay the committed key to the target after the text lands.** One Enter would then both commit
  and send, which is nicer in a chat box — and occasionally sends a half-finished message. The user
  chose consumed; safety agrees.
* **Every non-editing key commits, as Enter does.** This document said so until it was read back:
  §4.4 has the retraction and the reasoning. Short version — it is right for a preview and wrong
  for a surface the user is operating, because it fires an irreversible action from a key that
  means something else.
* **Every non-editing key passes through to the target, session continuing.** Ctrl+S would save,
  which is what the user wanted — but the target then receives input while uncommitted text is
  still queued for it, which is the ordering bug of §2 in a different costume. It also makes
  Ctrl+Z ambiguous between the pill and the application, with no way for either side to know
  which was meant.
* **Snapping the view but not the caret when speech arrives.** Two focus points and one 8-line
  window; the loser is a Backspace against a caret nobody can see. Moot now that §3.2 stops speech
  arriving during editing at all — kept here because it is the argument that made the caret-snap
  look necessary, and the snap is what made keeping the microphone open indefensible.
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
4. **How long does resuming take, and does it clip the first word?** §3.3 restarts a WASAPI capture
   mid-session, which nothing in the product does today — `begin_dictation` is the only caller of
   `start()`. Device initialisation is not free, and a user who says "resume" and starts talking
   immediately may lose the first syllable. **Measure it before designing around it.** If it is
   perceptible, the answer already exists in the tree: `progress.md` §8 records the same problem
   for the 4.2 s `windows-speech` model load, and the fix was to *hold the gesture* until the
   machine reported ready rather than to start something that was not.
5. **How long is too long?** There is no cap on the buffer. A twenty-minute session produces a
   buffer nobody wants to inject in one `SendInput` burst at ~1–2 ms/char (`plan.md` §1.7).
   The clipboard path is the obvious answer above some size, but the size is unmeasured.
6. **Does a mid-utterance suspend actually flush cleanly?** §3.3 claims the in-flight utterance
   finalises into the settled region via `FlushPendingAudio`. That is the endpoint path, so it
   should hold — but it has never been reached by a gesture, only by a timer, and `progress.md` §8
   is a list of paths that were right in every respect except being reached.
7. **Is `discontinuity` the right thing to send on resume**, or should the resume open a new
   utterance outright? A stateful-chunked backend (`progress.md` §3d's streaming arm, if it is
   built) may care about the difference, and neither has been tried.
8. **Does the mode survive a noisy room?** `progress.md` §4b says the noisy-room case is the one it
   does not cover. §3.2 helps — the microphone is shut during the half of the session where a
   stray word would do most damage — but it does nothing for a room that is noisy while the user
   is actually dictating.

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
* **`OverlayState::Editing` is the `Effect::SurfaceWarning` shape.** All seven raisers of that
  effect reached the overlay as one fixed 31-byte string, so a refusal during a model load was
  reported as a stability violation and nobody noticed for months. A new state that the pill
  renders but the tray does not — or that `set_tray_state` maps onto `Listening` because the
  switch has no case for it — reproduces that exactly, and §7.4 lists three separate rules that
  quietly stop holding when it does.
* **After §4.5 the hotkey no longer finalises, so `HotkeyPressed`'s Running branch is dead code.**
  `session.cpp:635` is the only place a session produces `stop` from a hotkey, and nothing will
  reach it. Left in place it is the `overlay_mode_for` situation exactly — correct, tested,
  called by nobody — except worse, because its *name* still claims the hotkey does this. Rename it
  and repoint it (§8), or delete the branch; do not leave it looking live.
* **`LocalEvent::EditingBegan` is the `SpeechDetected` shape**, and that one has already cost a
  session. `SpeechDetected` was raised from nowhere in `src/` — all four call sites were in
  `tests/` — so both silence timers hung off a value nothing computed, and the suite was green
  throughout. If the edit-key path posts a `HotkeyPost` that `drain_hook_queue` has no case for,
  the timer is never paused, and the only symptom is a session that occasionally ends itself
  mid-edit after two minutes. Check the call sites in `src/`, not the tests.

The habit: before trusting that something works, check which entry points the product actually
calls.

---

## 17. What is not done, and what a day of use has to answer

Written after the implementation, and deliberately separate from §15: those are questions the
design could not answer from the source, and these are things the implementation left behind.

### 17.1 Not implemented

* **§10.3's clipboard-rescue test.** "It needs a test before this ships, not after", and it does
  not have one. The path is `App::flush_deferred_commits` falling back to `set_clipboard_text`
  when `validate_target` fails, and it lives in `src/main.cpp`, which has no tests and which §11
  forbids putting anything interesting into. Testing it means extracting the decision — *inject,
  or rescue* — into somewhere pure, which is a change to a path this feature did not otherwise
  touch. It is the largest single gap here and it is the one most likely to bite, because §10.3
  is right that editing makes the path far more likely to run and makes it carry more text.
* **Dead keys.** `App::apply_edit`'s `char_for_key` calls `ToUnicodeEx` with the flag that
  forbids changing the kernel keyboard state — which it must, or a dead key typed into the pill
  would leave the layout's pending-diacritic state set and the *target* would receive the
  composed character the next time the user typed into it. The consequence is that a dead key
  produces nothing in the pill. Composing a diacritic needs the state that call is not allowed to
  keep, and a half-composed accent in the buffer would be worse than a key that did nothing.
* **`Overlay::scroll_by_lines` has exactly one caller**, the wheel. `editing.mouse = false`
  therefore also means "no scrollback", which §7.3 calls a trap: the arrow keys still reach the
  whole buffer, because caret-into-view scrolls for them, but there is no way to *read* back
  without moving the caret. A Page Up / Page Down pair in the Edit class would close it.
* **The settings round-trip found a gap it did not fix.** A settings *section* of the wrong type
  (`"session": 5`) is silently defaulted, because `Obj::member` resolves through
  `json::Value::find`, which answers nullptr for anything that is not an object. Recorded in
  `tests/test_settings.cpp` as a KD_INFO rather than fixed: it is a change to how every section
  is parsed, not to the block this feature added.

### 17.2 §15's questions, still open

Questions 1, 3, 4, 5, 6, 7 and 8 are unanswered, and none of them should be answered by guessing.
The two that most affect the design rather than the code:

* **Question 4 — resume latency.** §13 says to measure it *before* stage 2, and that was not
  done. `App::resume_dictation` restarts a WASAPI capture mid-session, which nothing in the
  product did before; if the first syllable is clipped, the answer already exists in the tree
  (progress.md §8's fix for the 4.2 s model load: hold the gesture until the machine reports
  ready). The `discontinuity` with `reason = "editing"` is already on the wire, so a stateful
  backend is being told the truth either way.
* **Question 2 — `SWP_FRAMECHANGED`.** Answered by doing it rather than by testing whether it was
  needed: `Overlay::set_click_through` calls `SetWindowPos` with the flag. One syscall per
  session boundary, and it removes the question.

### 17.3 What a day of use is expected to answer

* Does §4.1's mode feel wrong? The keyboard belonging to the pill is the price of the feature and
  the thing most likely to be rejected in use rather than in argument.
* Does §4.5's no-op hotkey read as broken? §4.5 names the one-line alternative — the hotkey pauses
  the microphone, symmetrically with resuming it — and says why it is not the default.
* Does the microphone button make §4.5 tolerable, or is the answer to bring the toggle back for
  the dictating state only?
* Does anything reach the fail-open path (§10.1) in ordinary use? It is written to be invisible
  when the UI thread is healthy; a session that silently stops accepting keys after two seconds
  would mean the heartbeat is not being stamped, and the log will not say so on its own.
