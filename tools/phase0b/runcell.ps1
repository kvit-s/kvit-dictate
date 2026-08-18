param([string]$Scenario = "S02", [string]$Inject = "sendinput", [int]$Repeat = 1,
      [string]$Text = "ref2", [int]$SettleMs = 0, [string]$Notes = "")

# One cell, for iterating on it. Same arrangement as sweep.ps1 — it dot-sources the same plan, so
# a cell cannot be measured here under conditions the sweep would not reproduce. TAKES OVER THE
# FOREGROUND, like the sweep.
#
# The earlier version of this script arranged nothing at all and passed neither --require-exe nor
# --text, so it measured whatever the target happened to be showing, with REF-1, which cannot
# measure anchor error (§5.9.1). Anything it produced should be read with that in mind.

$here = Split-Path -Parent $PSCommandPath
. "$here\arrange.ps1"

$cell = $plan | Where-Object { $_.id -eq $Scenario }
if (-not $cell) {
  Write-Output "no such cell '$Scenario' in the plan; have: $(($plan | ForEach-Object { $_.id }) -join ', ')"
  exit 2
}

# The cell's own reference string wins unless -Text was passed explicitly on the command line.
if ($cell.ContainsKey("text") -and -not $PSBoundParameters.ContainsKey("Text")) { $Text = $cell.text }

$t = Find-TestTarget

for ($i = 1; $i -le $Repeat; $i++) {
  Arrange $t $cell
  [SC]::Menu($t, $M.LogClear)
  Start-Sleep -Milliseconds 250
  [void][FG]::Force($t)
  Start-Sleep -Milliseconds 300
  $cls = [FG]::ClassOf([FG]::GetForegroundWindow())
  if ($cls -ne "KdTestTargetWindow") {
    Write-Output "REFUSING rep ${i}: foreground is '$cls'"
    continue
  }
  $note = "runcell rep $i; control=$($cell.control); $($cell.note); arranged from the baseline; foreground verified"
  if ($Notes -ne "") { $note = "$note; $Notes" }
  & "$bin\kd_probe.exe" --app A09 --scenario $Scenario --inject $Inject --text $Text `
      --require-exe kd_testtarget.exe --countdown 1 --repeat 1 --settle-ms $SettleMs --notes $note
}
