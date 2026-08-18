param([int]$Repeat = 3, [string]$Only = "", [string]$Text = "ref2")

# Measure the A09 row. THIS TAKES OVER THE FOREGROUND AND TYPES — see README.md. Run
# verify-arrange.ps1 first: it checks the same plan without injecting anything, and a cell that
# fails there is a cell whose measurement would mean nothing.

$here = Split-Path -Parent $PSCommandPath
. "$here\arrange.ps1"

$t = Find-TestTarget

foreach ($cell in $plan) {
  if ($Only -ne "" -and $cell.id -ne $Only) { continue }

  # A cell may name its own reference string. S05, S06 and S16 are defined by WHICH text is
  # injected, not by how the control is arranged, so -Text is a default rather than a mandate.
  $cellText = if ($cell.ContainsKey("text")) { $cell.text } else { $Text }

  Write-Output "=== $($cell.id) [$cellText] : $($cell.note)"
  # One probe invocation per repetition, with the scenario re-arranged in between. The probe's
  # own --repeat would leave the previous repetition's text in the control, so by rep 2 an
  # "empty field" is not empty, the line has wrapped and the control has scrolled - measuring a
  # different scenario under the first one's name.
  for ($i = 1; $i -le $Repeat; $i++) {
    Arrange $t $cell
    [SC]::Menu($t, $M.LogClear)
    Start-Sleep -Milliseconds 250
    [void][FG]::Force($t)
    Start-Sleep -Milliseconds 300
    if ([FG]::ClassOf([FG]::GetForegroundWindow()) -ne "KdTestTargetWindow") {
      Write-Output "  REFUSING rep ${i}: foreground lost"
      continue
    }
    & "$bin\kd_probe.exe" --app A09 --scenario $cell.id --inject sendinput --text $cellText --require-exe kd_testtarget.exe --countdown 1 --repeat 1 --notes "automated sweep rep $i; control=$($cell.control); $($cell.note); arranged from the baseline (scroll none, blinking caret, light theme, small font, all text cleared); foreground verified"
  }
}

# Leave the target in a neutral state.
[SC]::Menu($t, $M.ThemeLight)
[SC]::Menu($t, $M.ScrollNone)
[SC]::Menu($t, $M.CaretBlink)
