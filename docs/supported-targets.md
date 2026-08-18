# Supported targets — per-application matrix

**Status: v1 TEMPLATE. NO MEASUREMENTS HAVE BEEN TAKEN YET.**

Every result cell in this document reads `—`. That is not a placeholder for "works" or
"probably fine"; it means *nobody has run the probe on that cell*. Do not cite this file as
evidence of support until the cells contain measurements and the header below records a
measurement date and a probe build hash.

| | |
|---|---|
| Document version | v1 (template) |
| Phase | 0B — Windows feasibility spike (`plan.md` §3) |
| Measurements taken | *none* |
| Probe build | *n/a* |
| Machine | RTX 5090 / 64 GB / Windows 11 Pro (the target machine; see `plan.md` §5) |
| Results directory | `bench/results/phase0b/` |

---

## 1. Purpose

Two jobs, and they are different:

1. **A test plan.** Phase 0B (`plan.md` §3) is the critical gate: it decides whether the inline
   overlay is the baseline UX or whether a floating preview / finalize-only UX is. This document
   is the sheet that spike fills in. It is structured as tables with fixed row and column
   identifiers so that measurements can be *pasted in*, not written up.
2. **A published declaration.** Phase 4 ships this file. It is where a user finds out, before
   they are surprised by it, which applications kvit-dictate types into, which ones get a
   degraded experience, and which ones it refuses outright.

### 1.1 The declaration rule

> **Remote-session behaviour and elevated-target behaviour are DECLARED here, not discovered by
> users.**

This restates `plan.md` §1.7 ("Refuse to run on the secure desktop … Remote-session behaviour is
declared in the supported-target matrix, not discovered by users") and §1.7's UIPI paragraph
(elevated targets get "an explicit unsupported state — detected up front by comparing integrity
levels at session start … never a generic 'injection seemed not to work' guess").

The operational consequences, which the implementation must honour:

- An application that is not listed in §5 has **no declared behaviour**. The product's default
  for unknown targets is itself a declared tier (§4.4) — not silence.
- "Unsupported" must be surfaced *before* the user speaks, with a user-visible reason string.
  A session that produces nothing and explains nothing is a bug, not an unsupported target.
- If a measurement contradicts a declaration, the declaration is wrong and gets changed here.
  The code never papers over it at runtime.

---

## 2. Scenario definitions

The scenario axis is from `plan.md` §3 Phase 0B ("Scenario matrix, not just an app list"). IDs
are stable; never renumber, only append. `S11`/`S12` split plan.md's single "dark and light
theme" item into two rows because they are separately measurable.

| ID | Scenario | What it is, and why it breaks something |
|---|---|---|
| `S01` | Empty field | Nothing to diff against; source C has no prior text. Source B (last click) is imprecise into an empty field (§1.5). |
| `S02` | Mid-line insertion | The baseline case. Insertion point is interior; text to the right reflows. |
| `S03` | Selection replacement | The changed region includes deleted text, so its right edge is not the insertion point. |
| `S04` | Line wrap | Injection pushes the tail onto a new visual line. Bounding-box right edge lands on the *previous* line. |
| `S05` | Newline-only | Commit is a single `\n`. Little or no changed pixels; layout below shifts. |
| `S06` | Whitespace-only | Commit is spaces. Invisible change; an empty diff must **not** be read as injection failure (§1.5). |
| `S07` | Vertical scroll | Injection at the viewport edge auto-scrolls; the whole viewport repaints. Expected to score confidence ~0 and hold the previous anchor. |
| `S08` | Horizontal scroll | Long unwrapped line scrolls left; the caret stays put in screen space while all text moves. |
| `S09` | Autocomplete open | A popup overlaps the insertion point and repaints on every keystroke, adding changed regions that are not our text. |
| `S10` | Steady (non-blinking) cursor | Blink is a periodic diff source; a *steady* cursor is the control case that isolates it. Measure both: record blink behaviour in Notes. |
| `S11` | Dark theme | Diff thresholds tuned on dark-on-light must not invert. Also: subpixel AA differs. |
| `S12` | Light theme | As above, the other way. |
| `S13` | Mixed DPI | Target on a monitor at a different scale factor than the overlay's. Captures are physical pixels; the process is `PROCESS_PER_MONITOR_DPI_AWARE_V2` (§1.3). |
| `S14` | Multi-monitor | Target on a secondary monitor, including negative virtual-desktop coordinates. |
| `S15` | HDR | `BitBlt` from the desktop DC is expected to return black under HDR / multi-plane overlay; this row measures whether it does and whether the `Windows.Graphics.Capture` fallback works (§1.5, §8.1). |
| `S16` | RTL text | Insertion point is at the **left** edge of the changed region. Bidi runs make "rightmost changed pixel" actively wrong. |
| `S17` | Password field | Must be **refused**, not attempted (§1.7, §6). This row measures whether we *detect* it, not whether we type into it. |
| `S18` | Elevated target | Higher-integrity window. `SendInput` is silently dropped by UIPI and the return value does not say so (§1.7). This row measures whether we detect and refuse up front. |

### 2.1 Scenario construction notes

- `S17` and `S18` are **detection tests**. Pass = we refuse with a user-visible reason before any
  synthetic input is sent. A cell that reads "injection worked" for `S17` is a defect report.
- `S18`: launch the application elevated where it supports it. Where an application cannot
  meaningfully run elevated (e.g. the Explorer search box in the normal shell), record
  `n/a — cannot construct` in Notes rather than leaving the row blank.
- `S09` is **not only a scenario you construct**. Observed 2026-08-17, in the first end-to-end
  session anyone ran: Notepad3 (Scintilla) opened its own word-completion list in response to the
  injected keystrokes, unprompted, listing words already in the document. So an editor with
  autocompletion enters `S09` *by itself*, on ordinary dictation, and the popup then sits over the
  insertion point repainting on every commit. Two consequences worth measuring rather than
  assuming: whether the popup's repaints wreck source C's diff, and whether it swallows a
  subsequent keystroke — a completion list that treats Enter or Tab as "accept" would rewrite the
  user's text with a word they did not say. Construct the row deliberately as well, but record
  the spontaneous case in Notes.
- `S10`: Windows exposes a system caret blink time, and some applications draw their own caret
  with their own timing or none at all. Record which of the two you observed rather than assuming.
- Where a scenario cannot exist in an application at all, write `n/a` in the verdict column and
  say why in Notes. `n/a` is a result; blank is not.

---

## 3. Measurement definitions

The columns mean exactly this, and nothing looser.

### 3.1 Injection method that works

One of `SendInput` · `clipboard` · `both` · `none`. The two mechanisms are `plan.md` §1.7's
`SendInput` + `KEYEVENTF_UNICODE` and clipboard + `Ctrl+V`. Record the method that produced
**exact** text (§3.2); if both do, record `both` and put the preferred default in Notes with a
reason (speed, clipboard clobber, `Ctrl+V` binding conflicts in terminals).

Injection success is determined by reading the text back where readable (`tools/testtarget`,
and any target that will hand back its buffer) and otherwise by operator visual verification
against the reference string — **never** inferred from the pixel diff (`plan.md` §1.5:
"Injection success and anchor estimation are separate signals").

### 3.2 Unicode fidelity

The probe injects one fixed reference string, `REF-1`, defined once so results are comparable:

```
REF-1 (UTF-8, 8 segments separated by U+0020):
  1. ascii        : The quick brown fox
  2. identifiers  : llama.cpp --n-gpu-layers=999 C:\apps\foo_bar.exe
  3. punctuation  : "curly" 'quotes' — em-dash … ellipsis
  4. latin-accent : naïve café Ångström
  5. combining    : e + U+0301 (decomposed) vs U+00E9 (precomposed)
  6. cjk          : 日本語のテキスト
  7. emoji-zwj    : U+1F469 U+200D U+1F4BB (woman technologist)
  8. rtl          : العربية עברית
```

Grades:

| Grade | Meaning |
|---|---|
| `exact` | Byte-identical to `REF-1` after NFC-agnostic comparison (segment 5 is compared as-injected, not normalised — normalising would hide the exact defect it tests). |
| `lossy` | Text arrived but differs: dropped segment, mangled surrogate pair, split combining sequence, `?` substitution, reordered bidi run. **Record which segment and what happened** — "lossy" alone is not a result. |
| `fails` | Nothing arrived, or so little that the field is unusable. |

Segments 5, 7 and 8 are the ones expected to expose real defects; segment 2 is the one the
product exists for (`plan.md` §4: raw WER on `llama.cpp` is the point).

### 3.3 Anchor source that won

One of `A` (`GetGUIThreadInfo().rcCaret`) · `B` (last left-click before the hotkey) ·
`C` (commit differencing) · `none`, per `plan.md` §1.5. Record the source the combiner actually
selected, not the sources that were merely available; list also-rans in Notes. If the winner
flapped between sources during one commit, that is a `S07`-class hysteresis failure — record
`flap:A↔C` and treat the gate criterion "deterministic, non-flapping fallback" as at risk.

### 3.4 Anchor error in line heights

Signed error is not useful across apps; record **absolute error in units of the target's line
height**, so `0.4` means "within half a line". One line height is the gate unit (`plan.md` §3).

Ground truth:

- `tools/testtarget` reports its own caret rectangle to the probe over a side channel. This is
  the only cell family with **programmatic** ground truth and it calibrates everything else.
- Third-party applications have no such channel. Ground truth is the operator clicking the caret
  centre in the captured `after.png` in an annotation step. That introduces roughly a few pixels
  of operator error, which at a typical editor line height is a small fraction of a line but is
  **not zero**. Record the line height in pixels in Notes so the ratio can be re-derived.

Report the cell as `p50 / p95` over `N` repetitions (default `N = 20`), e.g. `0.2 / 0.6 (N=20)`.
A single sample is not a measurement — the 95 % criterion in the gate is a distribution claim.

### 3.5 Confidence score observed

The `0.00–1.00` score `anchor.cpp` computes from diff shape (`plan.md` §1.5). Report `p50 / min`
over the same repetitions. This column is what calibrates the low-confidence threshold; it is
recorded even for cells that fail, because the failing cells are the ones the threshold must
catch.

### 3.6 Verdict

`S` = Supported · `D` = Degraded · `U` = Unsupported · `n/a`. Definitions in §4. The per-scenario
verdict rolls up to the per-application tier by the rule in §4.5.

---

## 4. Support tiers

### 4.1 Supported

- Injection is **exact** for `REF-1` by at least one mechanism, reproducibly.
- Anchor lands within **one line height in ≥ 95 %** of that application's applicable scenarios.
- The inline overlay is rendered at the insertion point.
- Focus and integrity guards pass; the session never continues into a different target.

### 4.2 Degraded — floating pill, no inline overlay

- Injection is exact, **but** the anchor is unreliable in that target: below-threshold
  confidence, error beyond one line height, or a scenario class (RTL, horizontal scroll,
  autocomplete) where source C is structurally wrong.
- Behaviour: pending text is shown in a **floating pill near the target window** rather than at
  the insertion point. This is `plan.md` §1.5's defined degradation — "a defined state, not a
  bug" — and it must be *deterministic*: the same target and scenario must degrade the same way
  every time, with hysteresis preventing flapping between inline and pill within a session.
- The user is not asked to fix anything. The pill is a normal mode of the product.

### 4.3 Unsupported — explicit refusal with a user-visible reason

- Injection is impossible (`none`), lossy in a way that would corrupt the document, or forbidden
  by policy (§6).
- Behaviour: the session **does not start**. The tray and the overlay show a specific reason
  string, e.g. *"Elevated window — kvit-dictate cannot type into an administrator process."*
  Generic failure text is not acceptable; §1.7 exists precisely because UIPI failures are
  otherwise indistinguishable from bugs.
- Every unsupported cell in §5 must have a reason string drafted in its Notes, because the
  reason string is a shipped artifact, not an afterthought.

### 4.4 Unknown targets

An application not listed in §5 gets the **default tier**, declared here rather than emergent:

- Attempt injection with the global default mechanism.
- Run the anchor estimator; if confidence is below threshold — which is the expected case for an
  unmeasured target — degrade to the floating pill.
- Refuse outright only on the §6 conditions (integrity, secure desktop, password field), which
  are detectable without prior measurement.

So the worst case for an unmeasured application is Degraded, never silent corruption.

### 4.5 Roll-up rule

An application's tier is the **worst verdict among its applicable non-policy scenarios**, with
one exception: `S17` and `S18` are policy rows and are expected to be `U` everywhere; they do not
drag the application's tier down. An application whose `S17`/`S18` rows *fail to refuse* is
Unsupported regardless of everything else — that is a safety failure, not a quality one.

---

## 5. Results

**All cells are `—` pending measurement (see the status banner at the top of this file).**

Legend: **Inject** = `SendInput`/`clipboard`/`both`/`none` · **Uni** = `exact`/`lossy`/`fails` ·
**Src** = `A`/`B`/`C`/`none` · **Err** = |error| in line heights, `p50 / p95 (N)` ·
**Conf** = confidence `p50 / min` · **V** = `S`/`D`/`U`/`n/a`.

### 5.0 Per-application declaration (the published summary)

This is the table a user reads. It is derived from §5.1–§5.9 and must never be edited
independently of them.

| ID | Application | Tier | Default inject | Default anchor | Local session | Remote session (RDP) | Elevated instance |
|---|---|---|---|---|---|---|---|
| `A01` | VS Code | — | — | — | — | — | — |
| `A02` | Chrome | — | — | — | — | — | — |
| `A03` | Windows Terminal | — | — | — | — | — | — |
| `A04` | WSLg window | — | — | — | — | — | — |
| `A05` | Slack | — | — | — | — | — | — |
| `A06` | Explorer search box | — | — | — | — | — | — |
| `A07` | Office (Word) | — | — | — | — | — | — |
| `A08` | kvit-notes | — | — | — | — | — | — |
| `A09` | `tools/testtarget` | — | — | — | — | — | — |

**Remote session** is a declared column because of §1.1. Two distinct cases, measured separately
and both recorded in Notes: (a) kvit-dictate running *inside* an RDP session typing into an app
in that session; (b) kvit-dictate running on the console while the machine is RDP'd from
elsewhere. The named-pipe DACL is built on the **logon SID** specifically so that a different
terminal-services session cannot reach it (`plan.md` §1.2), so case (b) is expected to be a
refusal, but expected is not measured.

---

### 5.1 `A01` — VS Code

Electron; draws its own caret and text; async rendering. Construction: `S01` = new untitled
file; `S09` = trigger IntelliSense; `S16` = a `.txt` file with an Arabic line.

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.2 `A02` — Chrome

Chromium; the browser case that matters because **backspace can navigate** (`plan.md` §1.4) —
which is why nothing here may ever be repaired after commit. Construction: `S01` = an empty
`<textarea>` on a local test page; `S17` = an `<input type="password">`; `S09` = the omnibox
with suggestions open.

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.3 `A03` — Windows Terminal

`Ctrl+V` may be bound to something else or expected as `Ctrl+Shift+V` (`plan.md` §1.7); block
cursor; the shell prompt is destroyed by a stray backspace. Construction: `S01` = a fresh prompt;
`S08` = a long line in a narrow window; `S17` = a shell password prompt (**note**: this is not an
`ES_PASSWORD` control, so our detection heuristic will not see it — record how the product
behaves and whether that is acceptable).

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.4 `A04` — WSLg window

**The sharpest case, and the reason Phase 0B exists in this shape.** `Win+H` refuses these
windows; they expose no UIA text pattern; they are ordinary Windows windows for `SendInput` and
`BitBlt` (`plan.md` §1.5). The open question is whether `KEYEVENTF_UNICODE` survives translation
to X11 keysyms across the RDP pipe (`plan.md` §1.7) — clipboard paste is the likelier path.
**Record the answer to that question explicitly in §8.2, not only in this table.** Use a GUI
editor (e.g. `gedit`/`gnome-text-editor`) rather than a terminal so the scenarios are
constructible.

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.5 `A05` — Slack

Electron with a rich composer that rewrites text as you type (autoformat, emoji substitution,
mention popups). A target that **edits our committed bytes after we commit them** is a distinct
failure class from an anchor failure — record any such rewrite in Notes and flag it, because it
interacts with the §1.4 prefix invariant even though it is not our violation.

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.6 `A06` — Explorer search box

Small, single-line, results repaint live under the box, and the whole window is owned by the
shell. Scroll and wrap scenarios largely do not apply — mark them `n/a` with a reason rather
than leaving them open. Included because it is a real dictation destination and because a
single-line control with live filtering is a distinct anchor shape.

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.7 `A07` — Office (Word)

Record the exact build (Win32 desktop vs. Store vs. web-in-browser — they are different
applications for our purposes) in Notes. Spellcheck and grammar underlines repaint older text,
which is a named source-C failure mode (`plan.md` §1.5).

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.8 `A08` — kvit-notes

The sibling project (`..\kvit-notes`, Qt 6.10.1 rich text editor). A Qt target is a distinct
rendering stack from Win32, Electron and Office, and it is the one we can rebuild if a
measurement needs instrumenting.

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.9 `A09` — `tools/testtarget`

The **calibration target**. Classic edit, rich edit, multiline and password controls
(`plan.md` §7), plus a side channel that reports the true caret rectangle, the field's current
text, and the theme/DPI it is running under. Every other application's error figures are only
interpretable relative to this one: if `A09` cannot hit one line height, the estimator is broken,
not the target.

`S17` here is the **positive control for password detection** — a real `ES_PASSWORD` control that
our heuristic must catch.

| Scenario | Inject | Uni | Src | Err | Conf | V | Notes |
|---|---|---|---|---|---|---|---|
| `S01` empty field | — | — | — | — | — | — | — |
| `S02` mid-line insertion | — | — | — | — | — | — | — |
| `S03` selection replacement | — | — | — | — | — | — | — |
| `S04` line wrap | — | — | — | — | — | — | — |
| `S05` newline-only | — | — | — | — | — | — | — |
| `S06` whitespace-only | — | — | — | — | — | — | — |
| `S07` vertical scroll | — | — | — | — | — | — | — |
| `S08` horizontal scroll | — | — | — | — | — | — | — |
| `S09` autocomplete open | — | — | — | — | — | — | — |
| `S10` steady cursor | — | — | — | — | — | — | — |
| `S11` dark theme | — | — | — | — | — | — | — |
| `S12` light theme | — | — | — | — | — | — | — |
| `S13` mixed DPI | — | — | — | — | — | — | — |
| `S14` multi-monitor | — | — | — | — | — | — | — |
| `S15` HDR | — | — | — | — | — | — | — |
| `S16` RTL text | — | — | — | — | — | — | — |
| `S17` password field | — | — | — | — | — | — | — |
| `S18` elevated target | — | — | — | — | — | — | — |

---

### 5.10 Incidental observations — **not measurements**

Things seen while running the product for other reasons. They are recorded because they change
what the probe should look for, not because they are results: no cell above may be filled in from
this section, and none of it was taken with the probe or written down at the time by anything but
a human watching a screen.

| Date | Target | What was seen |
|---|---|---|
| 2026-08-17 | **Windows 11 Notepad** (`notepad.exe`, the WinUI/packaged one) | Paced `SendInput` (32 chars per batch, 1000 µs between batches) produced **garbled text**: long runs of one repeated character with most others missing — `can ngemall tttttttttttttttttto aaaaaaaaaaaad llllllll…`. The events themselves are correct by construction (`build_text_events` emits one down/up pair per UTF-16 unit, batch boundaries never split a pair, and the loop never resends), and the same build typed the same text into Notepad3 exactly, so this is the target's input pipeline and not ours. Two things to measure rather than assume: whether slower pacing (fewer chars per batch, longer delay) is enough, and whether the clipboard path is clean — the first thing to try, since a paste is one event instead of hundreds. |
| 2026-08-17 | **Notepad3** (Scintilla) | Typed the same text exactly, at the same pacing. Also opened its own word-completion popup unbidden — see the `S09` note in §2.1. |
| 2026-08-17 | **VS Code's integrated terminal** (`code.exe`) | Dictated into it incidentally and the text arrived correctly, with `SendInput` at the default pacing. The session log records `console=0`, which is **right**: VS Code's terminal is an xterm.js widget inside Electron, not a Win32 console, so it takes synthetic key events like any other Electron text surface. **This says nothing about real consoles.** `A03` (Windows Terminal / conhost) is the case `console_session`, the Ctrl+Shift+V paste chord and the whole console branch exist for, and it remains unmeasured — reading this row as "terminals work" is the specific mistake it is written to prevent. It also does **not** fill `A01`: that is a measured cell over the full scenario list, and this was one unscripted utterance into one surface of the application. |

| 2026-08-17 | **Windows Terminal** (`windowsterminal.exe`, standalone) | Three things, and the third is the useful one. (1) **Console detection works**: the session logged `console=1`, so the target was correctly classified and the console branch is live. (2) **The text arrived correctly** under `SendInput` at default pacing — no garbling of the Notepad kind. (3) **The overlay fell back to the floating pill, it appeared where the mouse pointer happened to be, and it never moved.** That is the §4.2 *Degraded* tier behaving exactly as designed, and for a console it is the expected outcome: there is no caret rectangle to be had from UIA or `GetGUIThreadInfo`, so source C is the only anchor available — and source C logged `empty diff after a visible commit` on every commit of that session, so it contributed nothing and confidence never rose above `min_confidence` 0.5. **Not a measurement**: `A03` is the cell for this target and it stays empty until the probe runs. What this row is for is that it makes `A03`'s likely verdict *Degraded rather than Unsupported* — the text lands, only the inline preview does not — and it says which of the two questions to put first. |

None of these targets is in the §5 application list as a *measured* row.

**A diagnostic gap this exposed, recorded so the next session does not re-derive it.** Across every
session run so far the log contains 8 `empty diff after a visible commit` and 4 `before-frame
capture failed` events from source C, **and not one line reporting a source-C success** — the
success path in `anchor.cpp` (`anchor_from_diff`, ~line 1162) logs nothing. So the log cannot
currently distinguish "source C ran and produced a good anchor" from "source C has never once
produced one", and the second would be a significant hole under Phase 0B, whose §3.3 column is
*which anchor source won*. One DEBUG line naming the winning source and its confidence would
settle it; until then, no claim either way belongs in this document. Notepad is a strong candidate for it: it is the
first thing anyone tries, it ships with Windows, and it is now known to break the default method.

These are not measurement failures. They are refusals the product makes on purpose, each with a
reason the user can read. They are listed here so the matrix is the single place that answers
"why did it not type?".

| Condition | Detection | Reason it is refused |
|---|---|---|
| **Secure desktop** | Desktop/session state checked at hotkey press. | The secure desktop is a separate desktop that ordinary user processes are not on. Our hook and our injection do not exist there. Attempting is not a degraded experience; it is nothing at all. |
| **UAC consent prompt** | Same check; the consent UI runs on the secure desktop. | As above, plus: a tool that could type into a consent dialog would be a privilege-escalation instrument. Refusing is a design property, not a limitation to fix. |
| **Lock screen** | Session-lock notification (`WTS`/session-change) puts the session in a refusing state and ends any live session. | Nothing the user says at the lock screen has a destination, and the mic must not be open there (`plan.md` §1.9: the mic is open only during a session). |
| **Password fields** | `ES_PASSWORD` style on the focused control where obtainable, plus the per-target heuristics recorded in `S17`. | Dictating a password aloud is the user's choice, but *routing it through a transcription pipeline and a screen-capture-based anchor* is not something we do quietly. Refuse and say so. **Honest limit:** this detection is best-effort — a custom-drawn or web password field may not be detectable, and a shell password prompt in a terminal certainly is not. `S17` measures the real coverage; whatever it shows is what gets declared here. |
| **Higher-integrity (elevated) targets** | Compare our process integrity level against the target process's at session start (`plan.md` §1.7). | UIPI blocks `SendInput` from a lower-integrity process into a higher-integrity window, and Microsoft notes the return value does not identify UIPI as the cause. So this must be detected *before* injection: the alternative is a session that appears to work and silently types nothing. |

Two things this table deliberately does **not** say:

- It does not promise to detect every password field. See the honest limit above.
- It does not offer "run kvit-dictate elevated" as a fix. Running a global keyboard hook and a
  synthetic-input engine at high integrity to type into elevated windows is a larger security
  decision than this project has made, and it would put the same hook in front of every elevated
  application on the machine. If it is ever revisited it gets its own written rationale.

---

## 7. The Phase 0B gate

Restated **verbatim** from `plan.md` §3, Phase 0B ("Explicit gate — decided before Phase 1
starts"), so that the pass/fail criterion lives next to the data it is applied to:

> - Exact Unicode injection in **every** declared supported target, with an explicit failure
>   state for unsupported and elevated ones.
> - Anchor placement within **one line height in ≥ 95 %** of supported scenarios.
> - Deterministic, non-flapping fallback when confidence is low.

And the consequence, also verbatim:

> **Meeting the gate** → inline overlay is the baseline UX. **Missing it** → floating preview or
> finalize-only becomes the baseline, and §1.4's commit policy is re-tuned accordingly. Either
> outcome is a success for this phase; not knowing is the only failure.

### 7.1 How each criterion is computed from §5

| Criterion | Computation | Result |
|---|---|---|
| Exact Unicode in every declared supported target | For every application whose §5.0 tier is `S`, every applicable non-policy scenario has `Uni = exact`. One `lossy` cell demotes that application; it does not fail the gate — it changes the declaration. | — |
| Explicit failure state for unsupported and elevated | Every `S17` and `S18` row, and every `U`-tier application, refused before injection with a user-visible reason string recorded in Notes. This one is **pass/fail, not a percentage**. | — |
| ≥ 95 % within one line height | Over all scenario-repetition samples in applications tiered `S`, the fraction with `Err ≤ 1.0`. Report the fraction, the sample count, and the applications excluded (with tier) — an unstated exclusion turns 95 % into a meaningless number. | — |
| Deterministic, non-flapping fallback | Over `M` repetitions (default 10) of each low-confidence scenario: identical fallback mode chosen every time, and zero inline↔pill transitions within a single session. Any `flap:` entry in §5 is a failure. | — |
| **Go / no-go on the inline overlay** | All four above. | — |

### 7.2 Recording the decision

When the numbers exist, the decision goes here as one paragraph: which criteria passed, which
did not, what the baseline UX consequently is, and what changes in `plan.md` §1.4 as a result.
Not a checkmark — a paragraph, because "missing it" is a legitimate outcome that has to be
explained rather than retried until it passes.

---

## 8. How to run the measurements

`tools/probe/` is built by the Phase 0B C++ work (`plan.md` §2). It is a minimal injector, focus
guard, overlay and anchor estimator — no ASR, fixed text only.

### 8.1 What the operator does

1. Build the probe. Record the commit hash; it goes in every result file and in this document's
   header.
2. Set up the scenario in the target application by hand: open the app, put it in the required
   theme/monitor/DPI state, place the caret, open the autocomplete popup, select the text to be
   replaced. **Scenario setup is manual on purpose** — automating it would mean driving the
   target with the same synthetic input we are trying to measure.
3. Run the probe for one cell:

   ```
   tools\probe\probe.exe --app A01 --scenario S02 --repeat 20 ^
                         --inject sendinput --text REF-1 ^
                         --out bench\results\phase0b
   ```

4. Click into the target field (this also seeds anchor source B — the click is part of the
   scenario, not a setup step), then press the probe's hotkey.
5. The probe injects `REF-1`, captures before/after frames, computes the anchor from all three
   sources, and writes the record. Repeat with `--inject clipboard` for the same cell; the two
   mechanisms are separate rows in the raw data and are collapsed into one §5 cell only at
   report time.
6. Annotate ground truth where the target has no side channel (§3.4): the probe reopens
   `after.png` and the operator clicks the caret. The click is stored, so re-deriving the error
   later never requires re-running the scenario.
7. Paste the aggregated row into §5. The aggregate is produced by the summariser, not by reading
   numbers off a screen.

### 8.2 What gets logged

One JSON record per (application, scenario, injection method, repetition), plus the frames:

```jsonc
{
  "probe_build": "<git sha>",
  "utc": "2026-08-16T12:34:56Z",
  "app": "A01", "scenario": "S02", "repeat_index": 3,
  "target": {
    "hwnd": "0x000A1B2C", "class": "Chrome_WidgetWin_1",
    "exe": "Code.exe", "file_version": "…",
    "pid": 1234, "integrity": "medium",
    "dpi": 144, "monitor": "\\\\.\\DISPLAY2",
    "monitor_rect": [2560, 0, 5120, 1440],
    "hdr": false, "session": "console"        // or "rdp"
  },
  "inject": {
    "method": "sendinput",
    "text_id": "REF-1", "bytes_sent": 187,
    "elapsed_ms": 214,
    "readback": "exact",                      // exact | lossy | fails | unavailable
    "readback_source": "side-channel",        // side-channel | operator
    "lossy_segments": []                      // e.g. ["emoji-zwj: surrogate split"]
  },
  "anchor": {
    "source_a": {"available": true,  "pt": [812, 430]},
    "source_b": {"available": true,  "pt": [800, 428], "age_ms": 1900, "invalidated": false},
    "source_c": {"available": true,  "pt": [1004, 431], "confidence": 0.82,
                 "diff_rect": [780, 420, 1010, 444], "diff_rows": 1,
                 "changed_px_fraction": 0.004},
    "winner": "C", "confidence": 0.82,
    "ground_truth": [1002, 431], "ground_truth_source": "operator",
    "line_height_px": 22, "error_px": 2.0, "error_lh": 0.09,
    "fallback_mode": "inline"                 // inline | pill | none
  },
  "capture": {
    "path": "bitblt",                         // bitblt | wgc
    "all_black": false, "wgc_indicator_seen": null,
    "before_ms": 3.1, "after_ms": 4.0, "after_polls": 2
  },
  "refusal": null,                            // or {"reason":"elevated-target","shown":"…"}
  "notes": ""
}
```

Fields that must never be silently omitted, because their absence is itself the finding:
`refusal`, `capture.path`, `capture.wgc_indicator_seen`, `inject.lossy_segments`,
`anchor.fallback_mode`.

The probe also answers two questions that Phase 0B owes `plan.md` §1.5/§1.7 and that no table
cell captures well. Both get written up in prose in §9 when known:

- The exact `Windows.Graphics.Capture` desktop path and **whether it shows a user-visible capture
  indicator** — a permanent yellow border around the desktop would be a UX blocker for the HDR
  fallback, not a detail.
- Whether `KEYEVENTF_UNICODE` survives the WSLg RDP→X11 keysym translation intact, and if not,
  whether clipboard paste does.

### 8.3 Where results land

```
bench/results/phase0b/
├─ <app>/<scenario>/<method>/<utc-stamp>/
│   ├─ result.json
│   ├─ before.png · after.png · diff.png     # gitignored
│   └─ probe.log
├─ summary.json          # one aggregated row per §5 cell
└─ gate.json             # the §7.1 computations
```

**Privacy (`plan.md` §1.9).** Phase 0B injects fixed text and records no speech, but the frames
are full-desktop captures and may contain anything that was on screen. PNGs are **gitignored**;
only `result.json`, `summary.json` and `gate.json` are committed, and the operator is expected to
run these on a desktop they are content to have on disk. Frames are what makes a disputed number
re-checkable, so they are kept locally rather than deleted — but they do not leave the machine.

---

## 9. Open questions

Recorded rather than guessed at, per repo policy. Each is closed by a Phase 0B measurement and
the answer is written here, in prose, next to the question.

1. **Does `WDA_EXCLUDEFROMCAPTURE` exclude our overlay from a `BitBlt` of the desktop DC?**
   `plan.md` §1.3 relies on it doing so, since a captured overlay would contaminate the very
   diff it is meant to inform. The affinity flag is documented as excluding the window from
   capture; whether that covers a GDI `BitBlt` from the desktop DC on this Windows build is
   something the probe must demonstrate empirically, not something to assume from the doc
   wording. Test: show the overlay over a static region, capture, assert the pixels are the ones
   behind it. — *Unanswered.*
2. **`GetGUIThreadInfo().rcCaret` coordinate space.** Documented as relating to `hwndCaret`;
   the probe must confirm the exact space (client vs. screen) and the behaviour when `hwndCaret`
   is null but `rcCaret` is non-empty, before source A's output is trusted in mixed-DPI cases.
   — *Unanswered.*
3. **`Windows.Graphics.Capture` desktop path and capture indicator.** See §8.2. `plan.md` §1.5
   explicitly refuses to assume this is transparent to the user. — *Unanswered.*
4. **WSLg `KEYEVENTF_UNICODE` survival.** See §8.2 and `plan.md` §1.7. — *Unanswered.*
5. **RDP case (b): can a console-session kvit-dictate be reached or observed from a remote
   session?** The logon-SID DACL (`plan.md` §1.2) is designed to prevent it. Verify rather than
   assert. — *Unanswered.*
6. **Does any target rewrite our committed bytes after we commit them?** (Slack autoformat,
   Office autocorrect.) This is not an ICER violation by us, but it changes what "committed"
   means in that target, and the declaration should say so. — *Unanswered.*

---

## 10. Change log

| Version | Date | Change |
|---|---|---|
| v1 | 2026-08-16 | Template created: scenarios, applications, tiers, gate, methodology. **No measurements.** |
