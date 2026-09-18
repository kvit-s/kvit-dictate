# Supported targets — per-application matrix

**Status: ONE REAL APPLICATION MEASURED, IN TWO SCENARIOS. 15 of 180 cells exist.**

Every result cell in the §5 per-application matrices reads `—`. That is not a placeholder for
"works" or "probably fine"; it means *nobody has run the probe on that cell*. The measurements that
exist live in **§5.9.2** (`A09`, the calibration target, 13 scenarios) and **§5.11** (`A10`,
Notepad3, 2 scenarios), not in the matrices, because the matrices' `Src` and `Uni` columns ask
questions the probe does not yet answer. Do not cite this file as evidence that kvit-dictate
supports any real application.

**The headline finding so far is a negative one.** Source C's *position* is accurate on a clean
append (0.07 line heights on both applications) and wrong by ten line heights on a mid-line
insertion — and its *confidence score cannot tell those two apart*. Across the fourteen scored
cells no threshold separates accurate estimates from inaccurate ones (§5.11.1), so plan.md §1.5's
deterministic degradation cannot yet be switched on. §9 question 7 says what has to change.

| | |
|---|---|
| Document version | v1 |
| Phase | 0B — Windows feasibility spike (`plan.md` §3) |
| Measurements taken | 2026-08-18 — `A09` 13 of 18 scenarios (§5.9.2); `A10` Notepad3, `S01`/`S02` (§5.11) |
| Probe build | `dev` (unstamped; `probe_build` in every record) |
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

- `tools/testtarget` reports its own caret rectangle to the probe over a side channel. It
  calibrates everything else, and it is the only source that can also report the text actually
  received, the theme and the DPI.
- **Any target backed by a real system caret is also self-measuring**, side channel or not:
  `GetGUIThreadInfo` read from *outside* the process returns what the target reports about
  itself. Checked against `tools/testtarget` two ways — the probe's external read of the
  pre-injection caret matched the side channel's to the pixel on 11/11 records, and running seven
  scenarios with `--no-side-channel` reproduced the side channel's `error_lh` **exactly on all
  seven** (`S01` 0.07, `S02` 9.33, `S03` 11.67, `S04` 0.07, `S07` 9.54, `S08` 39.00, `S16` 0.00).
  Records say which oracle they used in `ground_truth_source`.

  The limitation is real and is recorded per-record in `ground_truth_caveat`: on such a cell
  **source A is the oracle**, so nothing in it can assess source A's own accuracy and §3.3 —
  which anchor source won — cannot be answered from it. Only source C's error against the caret.

  It also requires the probe to wait for the caret to stop moving *before* capturing the
  after-frame, because `inject()` returns when events are sent and the frame poll stops at the
  first changed pixel. Without that wait the same cell scored 0.07, 0.87 and 1.94 line heights on
  three identical repetitions.
- **Surveyed, 2026-08-18, and the unattended path turns out to be narrow.** `tools/phase0b/
  caret-survey.ps1` reads `GetGUIThreadInfo` for every running window and types nothing. On this
  machine exactly one application exposed a caret: **Notepad3** (Scintilla). **Chrome, Edge, VS
  Code, File Explorer, Windows Terminal and SumatraPDF all reported none** — including `A06`,
  whose whole appeal was being a plain Win32 edit box. Win11's File Explorer is WinUI and its
  search field is not a classic `EDIT`.

  Read that as a **lower bound**, not a verdict: the survey reports the caret of whichever control
  has focus when a window is raised, which for a browser is the page and for Explorer is the file
  list. Chrome's omnibox and Explorer's search box would each need their focus chord sent first
  before "no caret" could be claimed of the application rather than of its default focus.

- **Operator annotation is needed only for targets that draw their own caret** — VS Code, Slack,
  terminals, Chromium content areas — but on the evidence above that is nearly all of them. Ground truth is then the operator clicking the caret centre
  in the captured `after.png`. That introduces roughly a few pixels of operator error, which at a
  typical editor line height is a small fraction of a line but is **not zero**. Record the line
  height in pixels in Notes so the ratio can be re-derived.

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

### 5.9.1 Open methodology defect — **REF-1 cannot measure §3.4**

Found on 2026-08-18 while making A09 self-measuring, and recorded before any sweep because it
decides what every cell in §3.4 would mean.

`REF-1` is a Unicode-fidelity torture string (§3.2's job) and it is being used as the injection
for the anchor-error column (§3.4) as well. It cannot serve both:

* **It does not fit the capture region.** 159 characters, ~1272 px in the target's Consolas 10 pt
  against a 900 px capture region. The diff is therefore clipped at the region's right edge, and
  "rightmost changed pixel" — which is how `anchor_from_diff` places the insertion point —
  returns the *boundary of the region*, not the end of the text.
* **It ends with an RTL run** (`العربية עברית`). After a bidi run the caret sits at the **left**
  of it, which is the failure `S16` exists to measure. With REF-1 as the universal injection,
  every row measures S16's failure mode underneath whatever it claims to be measuring.

Measured on A09/S01, reproducible to the pixel across repetitions: estimate pinned at x=900
(region edge), true post-commit caret at x=170, line height 15 → **48.68 line heights of error**,
of which `(900-170)/15 = 48.7` is entirely accounted for by the two effects above. Nothing in that
number is the estimator's doing, and A09 is the row §5.9 says calibrates all the others.

**Fixed, in the shape this section proposed and then some.** The probe now takes five reference
strings rather than one: **REF-1** for §3.2 fidelity, **REF-2** a short LTR string that fits inside
the capture region for §3.4 anchor error, **REF-3** its right-to-left counterpart so `S16` can
measure the case it is named for, and **REF-4**/**REF-5** — a lone newline and three spaces — so
`S05` and `S06` exist at all. Every record carries `text_id`, and `error_lh_valid_for_3_4` marks the
figures that may not be aggregated into §3.4 together with the reason.

### 5.9.2 A09 results — 13 of 18 cells, `sendinput`

Measured 2026-08-18 against `tools/testtarget`, with the side channel supplying ground truth and
`verify-arrange.ps1` confirming every arrangement beforehand (13 cells checked, 0 failing). Three
repetitions per cell; **every cell reproduced to the pixel across all three**, and no record is
flagged `suspect`. Raw records under `bench/results/phase0b/A09/`.

These are the records carrying `anchor.scored_against_previous`. Two earlier sweeps the same day are
superseded and their numbers must not be mixed with these — §5.9.3 and §5.9.4 say why each changed.
Both changes were to the *harness*; the product's estimator has not been touched.

| Cell | Scenario | text | error_lh (×3) | | conf | incumbent |
|---|---|---|---|---|---|---|
| `S16` | RTL text into an RTL control | REF-3 | **0.00** | exact | 0.4099 | source A caret |
| `S01` | empty field | REF-2 | **0.07** | exact | 0.3742 | source A caret |
| `S04` | line wrap | REF-2 | **0.07** | exact | 0.3742 | source A caret |
| `S10` | steady self-drawn caret | REF-2 | **0.12** | within 1 lh | 0.5000 | **none — no caret exists** |
| `S02` | mid-line insertion | REF-2 | 9.33 | over | 0.2083 | source A caret |
| `S11` | dark theme | REF-2 | 9.33 | over | 0.2083 | source A caret |
| `S12` | light theme | REF-2 | 9.33 | over | 0.2083 | source A caret |
| `S07` | vertical scroll (wrap at the bottom) | REF-2 | 9.54 | over | 0.1894 | source A caret |
| `S03` | selection replacement | REF-2 | 11.67 | over | 0.2941 | source A caret |
| `S06` | whitespace-only commit | REF-5 | 11.67 † | over | 0.3415 | source A caret |
| `S05` | newline-only commit | REF-4 | 21.02 † | over | 0.3091 | source A caret |
| `S08` | horizontal scroll | REF-2 | 39.00 | over | 0.1858 | source A caret |
| `S17` | password field | REF-2 | injection **refused** (`password_field`) | correct — refusal is the behaviour under test | — | source A caret |

† `S05` and `S06` carry `error_lh_valid_for_3_4: false`. They are **excluded from the published
§3.4 accuracy figure** — a distance computed from a diff that is deliberately near-empty measures
noise, and "how accurate is the estimator" is not a well-posed question for an invisible commit.
They are **included in the threshold analysis below**, because the question *that* asks is "should
this anchor be trusted", and for an invisible commit the answer is plainly no. Two different
questions, two different inclusion rules.

**Read `error_lh` as source C's error, not the product's.** The probe scores `anchor_from_diff`
unconditionally, while the product picks the best available source and ranks a real caret (0.95)
above any diff (≤0.90). In every cell except `S10`, `source_a.available` is true — these are Win32
edit controls with a real system caret — so the shipping product would have used the caret and been
exact. The probe's `winner` field applies its own rule (prefer C whenever the diff is non-empty) and
therefore does **not** answer §3.3; that is a defect in this harness, recorded rather than fixed.

#### 5.9.2.1 The scorer discriminates, but the margin is thin and shrinking

Sort by *true* error and the confidences still fall into two non-overlapping groups:

| | n | confidence range |
|---|---|---|
| accurate — true error < 1 line height | 4 | **0.3742 – 0.5000** |
| wrong — true error ≥ 1 line height | 8 | **0.1858 – 0.3415** |

**Any threshold in (0.3415, 0.3742] classifies all twelve correctly.** `min_confidence` defaults to
**0.50**, above the entire range of *good* scores, so enforcing it today rejects all twelve —
including the four the estimator gets right to within a tenth of a line height. `score_diff`
discriminates; the default is in the wrong place.

**But the margin halved when two cells were added.** With ten cells the separating band was
(0.2941, 0.3742], a margin of 0.0801. `S05` and `S06` — the invisible commits — arrived at 0.3091
and **0.3415**, and `S06` is now the nearest false positive with an error of 11.67 line heights.
The band is (0.3415, 0.3742]: **a margin of 0.033, down from 0.080, from two cells.**

That is the argument for not picking the number yet, and it is now an empirical argument rather
than a cautious one. Five `A09` scenarios remain unmeasurable here (`S09`, `S13`–`S15`, `S18`) and
no real application has been measured at all; on this trend one more cell could close the band
entirely, at which point no single threshold works and the scorer really does need retuning rather
than re-aiming.

Two structural weaknesses stand, neither of which breaks the separation:

- **The proximity term measures horizontal movement in units of line height.** A 25-character commit
  moves the insertion point ~11 line heights *horizontally*, so ordinary typing reads as a jump and
  every score is compressed toward the bottom of the range. Pinned by
  `anchor.s10_the_second_commit_is_scored_WORSE_not_better`. It compresses wrong answers harder,
  which is the only reason the separation survives.
- **An estimate with no incumbent scores higher than a corroborated one.** `kProxNoPrevious` is 0.85
  where a corroborated accurate estimate earns less, which is why `S10` lands at 0.5000 and `S01` —
  the same arrangement with a caret available — at 0.3742. Backwards.

And one cell to write: **`S17` scores 0.90** (`kMaxDiffConfidence`, one changed region) on a cell
where nothing was injected. The diff is the caret blinking. Injection is refused first so it reaches
nothing, but "a blinking caret alone scores maximum confidence" deserves a scenario of its own.

#### `S16`: the RTL relocation is right, and it is the misfires that are wrong

**0.00 line heights, three repetitions, with REF-3 — genuinely RTL text in an RTL control.** This is
the first measurement that has ever put right-to-left text in front of the estimator: REF-1 cannot
measure §3.4 at all (§5.9.1) and REF-2 is LTR by construction, so `S16`'s earlier figures described
an LTR run appended *after* RTL text, which is a different scenario and was labelled as such.

`diff_insertion_point` calls a change RTL when it grew **leftward** from the incumbent on the same
line and then reports the *left* edge of the changed band. Exactly right here, and exactly what §2
asks `S16` to check — "insertion point is at the **left** edge of the changed region. Bidi runs make
'rightmost changed pixel' actively wrong."

So the answer to §9 question 8 is **not** "remove the relocation". It is that the relocation is
correct for the case it was written for and the *trigger* is too loose. It also fires on two LTR
cells where the caret happened not to move:

- **`S08`, 39.00 line heights.** In an `ES_AUTOHSCROLL` control at its right edge the caret stays
  pinned while the text slides left, so `caret_before_injection == ground_truth` and the whole
  changed band lies to the left of the incumbent. Read as bidi; relocated to the far end.
- **`S07`, 9.54.** The same flip, landing accidentally nearer the truth than the alternative.

Both misfires share a signature the real RTL case does not: the incumbent caret **did not move**.
A genuine RTL commit moves the insertion point leftward *away* from the incumbent; a horizontally
scrolling LTR control leaves it exactly where it was while the text moves underneath. That
distinction is available at the point the decision is made and is not currently used.

REF-3 also lands a Unicode-fidelity data point for free: Hebrew survived `SendInput` +
`KEYEVENTF_UNICODE` verbatim, 28 bytes in, present in the readback.

#### `S05` and `S06`: the invisible commits arrive, and are not misread as failures

Both readbacks report the injection **present verbatim** from the side channel, and neither record
claims a failure — which is the rule under test (plan.md §1.5: "an empty diff does *not* prove
injection failed"). That part passes.

What they also show is that source C cannot place an invisible commit and does not know it:
`S06` inserts three spaces, moves the true caret 11.67 line heights' worth of distance, and scores
0.3415 — the highest score of any wrong cell in the row. `S05`'s newline reflows six changed
regions and still scores 0.3091. These are the cells that make the threshold question hard, and
they are the ones most likely to be common in real dictation.

#### `S10`: source C alone is accurate on a target that draws its own caret

**0.12 line heights — about 2 px — reproduced three times.** `S10` is the only cell with no system
caret, so source C is the only anchor available: the VS Code / Slack / terminal situation.

`S10` is deliberately `S01`'s arrangement with **one** variable changed — an empty field, a clean
append, and a control that never calls `CreateCaret`. And because there was no caret to centre on,
the capture region fell back to the whole window, which is exactly what `App::submit_before_frame`
does on the first commit into such a target. The bootstrap path was measured, not bypassed.

The probe deliberately does **not** feed the pane's own caret back in as an incumbent, even though
the side channel reports it: `tools/testtarget` knows where it drew that caret and the product
cannot, so using it would hand the estimator an oracle. That distinction is `caret_self_drawn` in
the state report, and getting it wrong inflated this cell's confidence by 0.11 in one intermediate
sweep.

What it does **not** say: this is a clean append into an empty field. `S02`'s reflow error and
`S07`'s region error both apply to a self-drawn target as much as to an edit control, and neither
has been measured without a system caret.

#### `S07`: the capture region, independent of the RTL flip

`capture_region_for` reaches 585 px behind the previous insertion point (900 × 220 at 96 dpi, point
at 65 % of the width) and the wrap moved the insertion point 728 px leftward, so the truth was
outside the captured rectangle and nothing inside it could have found it. `src/main.cpp`'s
`App::submit_before_frame` chooses the region by the same rule, so this is the product's behaviour.
The threshold is concrete: *an insertion point more than 585 px into a line at 96 dpi can wrap
outside the capture region.* Re-running with `--settle-ms 600` reproduced the figure exactly, so
none of it is a partial-repaint artefact.

#### The rest

`S02`/`S03` fail as §2 predicted when the scenarios were written: text to the right of an interior
insertion reflows, so the rightmost changed pixel is the end of the reflowed line rather than the
insertion point. `S11` and `S12` match `S02` to the pixel — **theme does not affect the estimate**.

`S04` at 0.07 is a **weaker result than it looks**: the long line already wraps when the scenario is
arranged, so the injection appends to the last visual line without causing a new wrap. It measures
an append inside a wrapped paragraph, not a wrap event. The wrap event is `S07`.

**Not measured:** `S09` needs an autocomplete popup, which `tools/testtarget` cannot raise;
`S13`/`S14`/`S15`/`S18` need display or elevation state that was not changed unattended.


### 5.9.3 The superseded sweep, and why five of its cells were arranged wrongly

The first automated sweep of 2026-08-18 produced these figures, which §5.9.2 replaces:

| Cell | superseded | now | why it changed |
|---|---|---|---|
| `S01` | 0.07 | 0.07 | unchanged — was arranged correctly |
| `S02` | 9.33 | 9.80 | unchanged in kind |
| `S03` | 11.67 | 11.67 | unchanged — was arranged correctly |
| `S04` | 9.33, 9.33, 49.95 | 0.07 | injected at offset 0, and never re-armed between repetitions |
| `S07` | 14.93 | 48.07 | nothing scrolled; now it does |
| `S08` | 17.40 | 0.24 | nothing scrolled; now it does |
| `S10` | 31.53, then no estimate | 0.12 | the pane scrolled 25 line heights per repetition |
| `S11`/`S12` | 9.33 | 9.80 | unchanged in kind |
| `S16` | 0.13-0.24 | 0.07 | the LTR run went in *before* the RTL text |
| `S17` | refused | refused | unchanged — correct both times |

Kept because the shape recurs, and because two of the old readings looked from the outside like
product defects and were not.

**`EM_SETSEL(-1, -1)` does not move an edit control's caret to the end.** `wParam = -1` means
"deselect"; the caret stays wherever the control already had it, which straight after
`SetWindowTextW` is offset 0. Four scenarios were arranged that way, so all four injected at the
*start* of their text:

* `S07` and `S08` never scrolled. `looks_like_scroll` was false because **nothing scrolled** — the
  control was already showing offset 0 and the insertion happened there. This reads in the raw
  records as a scroll-detection failure and is not one; scroll detection has still never been
  exercised. (`S07` also set the `ScrollOnInject` menu item, which only ever drove the self-drawn
  pane and could not scroll an EDIT at all.)
* `S04` never approached the wrap point.
* `S16` inserted its LTR run before the RTL text rather than after it, so it measures even less
  about bidi than §5.9.2 originally claimed.

**Scenario state leaked from one cell into the next.** `S07` switched the pane's "scroll on each
insertion" mode on and nothing switched it off, so `S10` ran with it live: each of REF-2's 25
characters scrolled the pane by a line, walking its content 25 line heights off the top of the
window. That is why `S10`'s ground truth came back at y = −226 and why repetitions 2 and 3
recorded no diff at all — there was nothing left on screen to change.

**`S04` was never re-armed between repetitions.** Its text menu item filled `edit-single` while
`S04` measures `edit-multi`, so each repetition typed into the previous one's leftovers. That is
the drift to 49.95 line heights on the third: by then the accumulated text had wrapped, and the
scenario under measurement was no longer `S04`.

The common thread has now recurred for five sessions running: **a contract
implemented on one side and never checked from the other.** The sweep declared what each cell
arranged; nothing ever asked the target whether that was what it had. `tools/testtarget` now
reports `sel_start` / `sel_end` / `text_length` in its state, `kd_probe --dump-state` prints that
state without injecting or capturing anything, and `verify-arrange.ps1` checks all eleven
arrangements against declared expectations before any sweep is allowed to mean anything.

### 5.9.4 The second superseded sweep — the probe scored against no incumbent anchor

The sweep that produced 48.07 for `S07`, 0.24 for `S08` and 0.07 for `S16` called
`score_diff(diff, region, std::nullopt)`. The product never does: at the moment a commit's diff is
scored there is always a published anchor to compare against — source A's caret at session start,
or the previous commit's own estimate. Passing `std::nullopt` disabled two things at once:

- the **proximity term**, which is the only part of `score_diff` that can notice the estimate has
  jumped, so every confidence in that sweep was missing its main discriminator; and
- `diff_insertion_point`'s **leftward-growth RTL check**, which needs an incumbent to compare
  against and which — once supplied — turns out to relocate the estimate on three cells.

The probe now builds the incumbent from the pre-injection caret and records it as
`anchor.scored_against_previous`. Records without that field are from the earlier sweeps and are
not comparable. The self-drawn pane is excluded on purpose: `tools/testtarget` reports the caret it
drew itself, and feeding that back would give the estimator an oracle the product cannot have.

The lesson is the same one §5.9.3 records, one level further in: **a harness that is not faithful to
the product measures a different function.** The first sweep arranged the wrong scenarios; this one
arranged them correctly and then scored them with a call the product never makes.

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

| 2026-08-17 | **Windows' own dictation (`Win+H`)** — *third-party behaviour, for comparison* | **Shows a live preview in VS Code's integrated terminal, and none at all in Windows Terminal** — in the real console nothing appears until the utterance is finalised, and then the final text is applied in one go. The contrast is the whole value of this row: Microsoft's first-party dictation, with whatever private access it has, draws the same line we do, in the same place, between an Electron text surface and a real Win32 console. It is the closest thing to a control this project can get, and it says **the console is the constraint, not our implementation**. Note we degrade *less* badly there than it does — a floating pill with live text against no preview at all. This is the main argument for capping `A03`'s ambition at the §4.2 *Degraded* tier rather than spending the Windows.Graphics.Capture work (§6) on inline preview inside consoles. Not a measurement of anything we built and no cell may cite it; the mechanism was not investigated (likely TSF rather than synthetic input), so this is an observation about outcomes only. |
| 2026-09-17 | **VS Code** (`code.exe`) under the `clipboard` method, and **`tools/testtarget`**'s plain Win32 `EDIT` | The user reported pastes delivering the *previous* clipboard item instead of the dictation. Cause: the restore ran 60 ms after the chord and Electron's paste — browser process to renderer and back — is routinely slower than that, so the restore won. Fixed by deferring the restore to a `WM_TIMER` and waiting `inject.paste_settle_ms`, now 1200 ms. Measured against `testtarget` afterwards: five of five pastes verbatim, our text on the clipboard for 1203–1218 ms each time. **Delayed rendering was tried first and rejected here**: against the same `EDIT` control it produced no paste at all, because **Ditto** (the clipboard manager on this machine, pid confirmed) answers the one `WM_RENDERFORMAT` request before the target does. This is a measurement of the mechanism, not of `code.exe`, which still has no measured row. |

None of these targets is in the §5 application list as a *measured* row.

**A diagnostic gap this exposed, recorded so the next session does not re-derive it.** Across every
session run so far the log contains 8 `empty diff after a visible commit` and 4 `before-frame
capture failed` events from source C, **and not one line reporting a source-C success** — the
success path in `anchor.cpp` (`anchor_from_diff`, ~line 1162) logs nothing. So the log cannot
currently distinguish "source C ran and produced a good anchor" from "source C has never once
produced one", and the second would be a significant hole under Phase 0B, whose §3.3 column is
*which anchor source won*. **Both halves are now closed.** `anchor.cpp`'s `publish()` logs the
winning source and its confidence whenever the source changes, and logs a losing candidate when a
*different* source is rejected — so "source C works but the caret outranks it" and "source C
produced nothing" are distinguishable in the field. And `tests/test_anchor.cpp` now drives
`AnchorEstimator` end to end against a scripted capture: **source C demonstrably can publish a
usable anchor with no caret available**, so the empty diffs in this log are the targets not
changing visibly in the captured region, not a dead code path. What remains unknown is how often
it succeeds against *real* targets, which is the probe's job. Notepad is a strong candidate for it: it is the
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

### 5.11 `A10` — Notepad3, the first real application measured

Measured 2026-08-18, `sendinput`, REF-2, three repetitions per cell, ground truth from the target's
own system caret (§3.4). Two scenarios: without a side channel the document can only be arranged by
typing into it, so only the scenarios reachable that way exist. `tools/phase0b/notepad3.ps1` is the
harness; it launches a fresh `Untitled` window, refuses to proceed if the title carries Notepad3's
unsaved-changes asterisk, and pins that window's `HWND` into every injection.

| Cell | Scenario | error_lh (×3) | confidence (×3) |
|---|---|---|---|
| `S01` | empty document | **0.07, 0.07, 0.07** | 0.3962, 0.5021, 0.5021 |
| `S02` | mid-line insertion, caret 20 of 45 | **10.27, 10.27, 10.27** | 0.3956, 0.3956, 0.3956 |

The position figures agree with `A09` closely — 0.07 against 0.07 for a clean append, 10.27 against
9.33 for a mid-line insertion — which is the first evidence that the calibration target predicts a
real one at all. That is the good news and it is worth stating plainly: **source C behaves the same
way in Scintilla as it does in a Win32 edit control.**

#### 5.11.1 The confidence threshold does not survive a second application

§5.9.2.1 found that across twelve `A09` cells, every accurate estimate scored above every
inaccurate one, and that any threshold in (0.3415, 0.3742] classified all twelve correctly. It also
warned that the margin had halved to 0.033 when two cells were added, and that "one more cell could
close the band entirely, at which point the scorer needs retuning rather than re-aiming."

That is what happened, on the first real application, in the first two cells:

| | cell | true error | confidence |
|---|---|---|---|
| **wrong** | `A10/S02` | 10.27 line heights | **0.3956** |
| accurate | `A09/S01`, `A09/S04` | 0.07 line heights | 0.3742 |
| accurate | `A10/S01` (worst repetition) | 0.07 line heights | **0.3962** |

`A10/S02` is wrong by ten line heights and scores **higher than both of `A09`'s accurate cells**, and
higher than four of the five accurate cells overall. Against the one accurate cell it does not beat
— its own application's `S01`, on that cell's worst repetition — it loses by **0.0006**.

**No threshold separates accurate from inaccurate estimates across the fourteen cells now measured.**
The answer to §9 question 7 is therefore not a number. Re-aiming `min_confidence` is not enough, and
the two structural weaknesses §5.9.2.1 listed as "not blocking" are now the whole problem.

#### 5.11.2 A real editor's own autocomplete moves the score by a third of the old margin

`A10/S01` scored 0.3962 on its first repetition and 0.5021 on the other two, with identical
arrangement, identical injection and an identical position error of 0.07 line heights. The
difference is in the diff: 27 changed rows over 3 regions on the first, 15 over 2 on the rest.

That is almost certainly §2.1's `S09` note happening by itself — "Notepad3 (Scintilla) opened its own
word-completion list in response to the injected keystrokes, unprompted". An editor with
autocompletion enters `S09` on ordinary dictation, and the popup's repaint costs **0.106 of
confidence** on an otherwise perfect estimate.

For scale: the entire separating band that twelve `A09` cells supported was 0.033 wide. A single
unprompted popup in a real editor moves the score by three times that. Whatever replaces the current
scorer has to be stable against a target repainting things that are not our text, because real
editors do it unasked.

#### 5.11.3 What A10 does not say

- **Two scenarios, not eighteen.** Nothing here about scroll, wrap, selection replacement, theme, or
  the invisible commits, because arranging them needs either a side channel or much more
  keystroke plumbing.
- **No readback.** There is no side channel, so `inject.readback` is null and the record carries the
  operator TODO. The text arriving correctly is *not* established by these records — only the
  caret's final position is. §3.1's rule stands: injection success is never inferred from pixels,
  and it has not been established here by anything else either.
- **Source A is the oracle** (§3.4), so these cells say nothing about source A's own accuracy and
  cannot answer §3.3.

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

7. **What should `AnchorConfig::min_confidence` be?** — **Answered, and the answer is that no value
   works.** Measuring a real application closed the question in the way §5.9.2.1 warned it might.

   On the calibration target alone the picture looked good: across twelve `A09` cells every estimate
   accurate to within one line height scored 0.3742–0.5000 and every inaccurate one 0.1858–0.3415,
   so any threshold in (0.3415, 0.3742] classified all twelve correctly. The margin was 0.033 and
   shrinking — it had been 0.080 before `S05` and `S06` were added.

   `A10` (Notepad3) closed it on its first two cells. **`A10/S02` is wrong by 10.27 line heights and
   scores 0.3956**, above both of `A09`'s accurate cells (0.3742) and within **0.0006** of the worst
   repetition of `A10`'s own accurate cell. Across the fourteen cells now measured, **no threshold
   separates accurate estimates from inaccurate ones** (§5.11.1).

   So `min_confidence` cannot be fixed by choosing a better number, and plan.md §1.5's degradation
   gate — written, tested, and deliberately not called in `App::apply_overlay_mode` — stays
   uncalled for a stronger reason than before. Three things have to happen first, and they are
   ordered:

   1. **`score_diff` needs to distinguish the failure it currently cannot.** The one that matters is
      a mid-line insertion: text to the right reflows, so the rightmost changed pixel is the end of
      the reflowed line rather than the insertion point (`A09/S02` 9.33, `A10/S02` 10.27, both
      scoring in the accepted range). A diff whose changed band *starts* at the incumbent anchor and
      extends right is an append; one that starts left of it and extends past it is a reflow. That
      distinction is geometric, available, and unused — the same shape as the fix open question 8
      identifies for the leftward-growth trigger.
   2. **Whatever replaces it has to be stable against repaints that are not our text.** `A10/S01`
      moved 0.106 between identical repetitions because Notepad3 opened its own word-completion
      popup unbidden (§5.11.2) — three times the entire margin the `A09`-only analysis supported.
   3. **Only then is a threshold worth choosing**, and it should be chosen over at least two
      applications, because one application supported a band that a second destroyed.

   The two weaknesses previously logged here as "not blocking" are part of the same problem and are
   retained: the proximity term measures horizontal movement in units of line height, so a
   25-character commit reads as an ~11-line-height jump and every score is compressed toward the
   bottom of the range; and an estimate with **no** incumbent scores `kProxNoPrevious` 0.85, higher
   than a corroborated accurate one.

   **Meanwhile the pre-gate defect ships**: the overlay draws at whatever anchor it is handed. On
   the evidence above that is right about as often as it is wrong, and the product cannot currently
   tell which.

8. **Should the leftward-growth RTL heuristic relocate the insertion point, or only cost
   confidence?** — **Answered: relocate. The trigger is what needs narrowing.** `S16` measured with
   REF-3 — genuinely right-to-left text in an RTL control, the first time any measurement has put
   RTL text in front of the estimator — gives **0.00 line heights of error, three repetitions**. So
   `diff_insertion_point` reporting the *left* edge of the changed band is exactly right for the
   case it was written for, and removing it would break the one cell it gets perfect.

   The remaining defect is that the trigger also fires on two LTR cells: `S08` at **39.00** line
   heights, where an `ES_AUTOHSCROLL` caret stays pinned at the right edge while the text slides
   left, and `S07` at 9.54 where the same flip lands accidentally nearer the truth. Both misfires
   share a signature the real RTL case does not: **the incumbent caret did not move.** A genuine RTL
   commit moves the insertion point leftward *away* from the incumbent; a horizontally scrolling LTR
   control leaves it exactly where it was while the text moves underneath. That distinction is
   available where the decision is taken and is not currently used.

   — *Deprioritised, 2026-08-18.* Two reasons, in order of weight. **This product is English-only**
   (`bench/CANDIDATES.md`: "English-only is our entire scope and multilingual capacity is dead
   weight"; plan.md §694 lists multi-language as a non-goal), so the case the relocation gets right
   will never occur and only the misfires matter. And **the misfires are already handled** by
   question 7's gate: `S07` scores 0.1894 and `S08` 0.1858, both far below any threshold in the
   candidate band, so once the gate exists neither wrong position is ever drawn. A wrong estimate
   that is correctly labelled as untrustworthy is not a bug.

   Note also that deleting the branch is **not** the safe simplification it looks like. It would
   take `S08` to 0.24 — that is the figure from the sweep with no incumbent, where the check could
   not fire — but it would also remove `kRtlPenalty` (0.65), which is part of what pushes `S07` and
   `S08` below the threshold in the first place. `S07`'s position without the flip is 48.07 line
   heights out. Raising a badly wrong anchor's confidence into the *accepted* band is strictly worse
   than today.

   Worth keeping in mind if this is ever picked up: the trigger is misnamed rather than misplaced.
   Its own comment concedes it fires on right-aligned LTR text as well as bidi, and right-aligned
   English fields (numeric inputs, spreadsheet cells) are in scope even though RTL is not.

9. **Should the capture region widen, re-centre, or stay as it is?** — *Measured; the decision is
   open.* `capture_region_for` reaches 585 px behind the previous insertion point (900 × 220 at
   96 dpi, point at 65 % of the width). `A09`/`S07`'s wrap moved the insertion point 728 px
   leftward, putting the truth outside the captured rectangle, so no estimator could have found it.
   `src/main.cpp`'s `App::submit_before_frame` chooses the region by the same rule, so this is the
   product's behaviour and not the harness's. The concrete threshold: *an insertion point more than
   585 px into a line at 96 dpi can wrap outside the capture region.* Widening costs capture time
   on every commit; the 65/35 rightward bias assumes text grows rightward, which is exactly
   backwards when a wrap fires; or a low score here is the estimator correctly reporting "I do not
   know" and question 7's gate should hold the previous anchor.

---

## 10. Change log

| Version | Date | Change |
|---|---|---|
| v1 | 2026-08-16 | Template created: scenarios, applications, tiers, gate, methodology. **No measurements.** |
