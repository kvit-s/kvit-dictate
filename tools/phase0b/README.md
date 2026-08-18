# Phase 0B automation for `A09` (`tools/testtarget`)

Drives `kd_probe` against `tools/testtarget` unattended: arranges each scenario through the
target's side channel, verifies focus, runs the probe, and leaves the target in a neutral state.
Only `A09` can be driven this way — it is the one target with a side channel and menu commands to
arrange itself (docs/supported-targets.md §3.4, §5.9).

## Verify the arrangement before you measure it

```powershell
tools\phase0b\verify-arrange.ps1          # all cells
tools\phase0b\verify-arrange.ps1 -Only S10
```

This arranges each cell exactly as the sweep does, then asks the target what state it is actually
in and checks it against the cell's declared `expect` block: which control holds the focus, where
the caret sits **in the text**, what the text is, the theme, whether a system caret exists, and
that no previous cell's pane scroll mode leaked in. It injects nothing, captures nothing and never
touches the foreground, so unlike the sweep it is safe to run while using the machine.

It exists because the first eleven measured cells included five whose arrangement did not match
their name, and nothing in the pipeline noticed — see docs/supported-targets.md §5.9.3. **Run it
before any sweep.** A cell that fails here produces a measurement that means nothing.

```powershell
# every scenario this harness can arrange, three repetitions, anchor-error injection
tools\phase0b\sweep.ps1 -Repeat 3 -Text ref2

# one scenario
tools\phase0b\sweep.ps1 -Only S02 -Repeat 3 -Text ref2

# the Unicode-fidelity pass instead (§3.2)
tools\phase0b\sweep.ps1 -Repeat 3 -Text ref1
```

`kd_probe.exe --status` prints the resumable coverage matrix. Records land in
`bench/results/phase0b/<app>/<scenario>/<method>/<stamp>/result.json`.

## It takes over the foreground, and that is not optional

The probe injects into whatever holds the foreground when its countdown expires, so the sweep
forces `tools/testtarget` to the front (`focus.ps1`, via `AttachThreadInput`) before each
repetition. **Do not run this while using the machine for anything else.** Windows will be pulled
in front of your work for as long as the sweep runs.

What it will *not* do is type into the wrong window. `--require-exe kd_testtarget.exe` is passed on
every invocation and is checked twice inside the probe — once against the captured target and again
immediately before the events go out — and a mismatch skips the repetition with nothing typed. That
guard exists because an earlier version of this sweep typed REF-1 into the operator's editor: the
launching script had checked focus, but it checked before a one-second countdown in a separate
process, and the gap is inside the probe.

Separately, each record carries a `validity` block saying whether the target held the foreground at
the before-frame and the after-frame. A repetition that lost focus and got it back is marked
`suspect` — the capture is of a screen *rectangle*, so an overlapping window puts someone else's
pixels in the diff and produces a record that looks perfectly normal. Exclude suspect records from
any aggregate.

## Which scenarios this covers

`$plan` in `arrange.ps1` holds one entry per scenario the harness can arrange: S01, S02, S03, S04,
S07, S08, S10, S11, S12, S16, S17. Deliberately absent:

* **S05, S06** — need an injection other than REF-1/REF-2 (newline-only, whitespace-only). The
  probe has no `--text` value for them.
* **S09** — needs an autocomplete popup over the insertion point; `tools/testtarget` cannot raise
  one. Notepad3 does it unbidden, which is what §2.1's `S09` note is about.
* **S13, S14, S15, S18** — mixed DPI, multi-monitor, HDR and elevation. All need machine state that
  should be changed deliberately by a human, not by a script running unattended.

## Files

* `arrange.ps1` — the scenario table (`$plan`), the baseline every cell starts from, and
  `Arrange`. Dot-sourced by both of the below, so what gets verified is literally what gets
  measured. Each cell re-arms from the baseline before **every** repetition: the probe's own
  `--repeat` would leave the previous repetition's text in the control, so an "empty field" is
  only empty the first time, and a mode one cell switches on is a mode the next cell inherits.
* `verify-arrange.ps1` — checks the arrangement without injecting. Run this first.
* `sweep.ps1` — the measurement loop. Takes over the foreground.
* `runcell.ps1` — one scenario, for iterating on a single cell.
* `focus.ps1` — the foreground helper.
