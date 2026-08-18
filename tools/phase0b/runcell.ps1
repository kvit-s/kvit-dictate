param([string]$Scenario = "S02", [string]$Inject = "sendinput", [string]$Control = "edit-multi", [int]$Repeat = 1)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$here\focus.ps1"

$bin = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build-windows-msvc-debug\bin\Debug"

if (-not (Get-Process kd_testtarget -ErrorAction SilentlyContinue)) {
  Start-Process -FilePath "$bin\kd_testtarget.exe" | Out-Null
  Start-Sleep -Seconds 2
}
$t = [FG]::Find("KdTestTargetWindow")
if ($t -eq [IntPtr]::Zero) { Write-Output "no testtarget window"; exit 1 }

[void][FG]::Force($t)
Start-Sleep -Milliseconds 500

# Refuse to inject unless the target really has focus. The probe types into whatever is focused
# during its countdown; this is the check that keeps REF-1 out of the user's work.
$fg = [FG]::GetForegroundWindow()
$cls = [FG]::ClassOf($fg)
if ($cls -ne "KdTestTargetWindow") {
  Write-Output "REFUSING: foreground is '$cls', not KdTestTargetWindow"
  exit 2
}
Write-Output "foreground confirmed: $cls"

& "$bin\kd_probe.exe" --app A09 --scenario $Scenario --inject $Inject --countdown 1 --repeat $Repeat --notes "automated run; foreground verified as KdTestTargetWindow before launch; control=$Control"
