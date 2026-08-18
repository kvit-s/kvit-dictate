# Check that every cell in the A09 plan arranges the scenario its name claims — WITHOUT injecting
# anything, capturing anything, or taking the foreground. It is therefore safe to run while the
# machine is in use, which sweep.ps1 is not.
#
# This exists because the first eleven measured cells were arranged wrongly and the records did
# not say so. EM_SETSEL(-1, -1) does not move an edit control's caret to the end, so S04, S07,
# S08 and S16 all injected at offset 0: S07 and S08 never scrolled, S04 never reached the wrap
# point, and S16 put its LTR run before the RTL text instead of after it. Every one of those is
# visible in the arranged state before a single character is typed. Run this first, always.

param([string]$Only = "")

$here = Split-Path -Parent $PSCommandPath
. "$here\arrange.ps1"

$t = Find-TestTarget
$fail = 0
$checked = 0

foreach ($cell in $plan) {
  if ($Only -ne "" -and $cell.id -ne $Only) { continue }

  Arrange $t $cell
  Start-Sleep -Milliseconds 200

  $raw = & "$bin\kd_probe.exe" --dump-state
  if ($LASTEXITCODE -ne 0) { Write-Output "[$($cell.id)] FAIL: $raw"; $fail++; continue }
  $s = $raw | ConvertFrom-Json
  $e = $cell.expect

  $problems = @()

  if ($s.focus.name -ne $e.focus) {
    $problems += "focus is '$($s.focus.name)', expected '$($e.focus)'"
  }

  # The caret offset, which is the thing that was wrong and unreported for four cells.
  if ($e.ContainsKey("sel")) {
    $parts = $e.sel.Split(":")
    if ([int]$s.focus.sel_start -ne [int]$parts[0] -or [int]$s.focus.sel_end -ne [int]$parts[1]) {
      $problems += "selection is [$($s.focus.sel_start),$($s.focus.sel_end)], expected [$($parts[0]),$($parts[1])]"
    }
  } else {
    $want = if ($e.caret_at -eq "end") { [int]$s.focus.text_length }
            elseif ($e.caret_at -eq "start") { 0 }
            else { [int]$e.caret_at }
    if ([int]$s.focus.sel_start -ne $want -or [int]$s.focus.sel_end -ne $want) {
      $problems += "caret at [$($s.focus.sel_start),$($s.focus.sel_end)] of $($s.focus.text_length), expected $want ($($e.caret_at))"
    }
  }

  if ($e.text_head -ne "" -and -not $s.text.StartsWith($e.text_head)) {
    $head = if ($s.text.Length -gt 40) { $s.text.Substring(0, 40) } else { $s.text }
    $problems += "text starts '$head', expected '$($e.text_head)'"
  }
  if ($e.text_head -eq "" -and -not $e.ContainsKey("sel") -and $e.caret_at -eq 0 -and $s.text -ne "") {
    $problems += "text should be empty, holds $($s.text.Length) chars — a previous cell leaked into this one"
  }

  if ($e.ContainsKey("theme") -and $s.theme -ne $e.theme) {
    $problems += "theme is '$($s.theme)', expected '$($e.theme)'"
  }

  # S10's whole point: this control must NOT expose a system caret, or it is not measuring the
  # VS Code / Slack / terminal case at all.
  if ($e.ContainsKey("no_system_caret") -and $s.caret.system_caret_present) {
    $problems += "a system caret is present; S10 requires source A to be unavailable"
  }

  # No cell may inherit the pane's scroll mode. S10 is the one this actually broke, but the leak
  # is a property of the harness rather than of any one cell, so check it everywhere.
  if ($s.scroll_mode -ne "none") {
    $problems += "pane scroll_mode is '$($s.scroll_mode)', expected 'none'"
  }

  $checked++
  if ($problems.Count -eq 0) {
    Write-Output "[$($cell.id)] ok    focus=$($s.focus.name) caret=$($s.focus.sel_start)/$($s.focus.text_length) theme=$($s.theme) sys_caret=$($s.caret.system_caret_present)"
  } else {
    $fail++
    Write-Output "[$($cell.id)] FAIL"
    foreach ($p in $problems) { Write-Output "         $p" }
  }
}

Write-Output ""
Write-Output "$checked cells checked, $fail failing"
if ($fail -gt 0) { exit 1 }
