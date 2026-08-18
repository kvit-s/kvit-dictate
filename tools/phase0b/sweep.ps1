param([int]$Repeat = 3, [string]$Only = "", [string]$Text = "ref2")

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$here\focus.ps1"

Add-Type @"
using System; using System.Runtime.InteropServices;
public class SC {
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out UIntPtr r);
  [StructLayout(LayoutKind.Sequential)] public struct CDS { public IntPtr dwData; public int cbData; public IntPtr lpData; }

  // Fire-and-forget side-channel command. wParam = 0 means "do not reply", which
  // tools/testtarget honours (it only replies when the requester handle is a window).
  public static void Command(IntPtr target, string cmd) {
    byte[] b = System.Text.Encoding.UTF8.GetBytes(cmd);
    IntPtr buf = Marshal.AllocHGlobal(b.Length);
    Marshal.Copy(b, 0, buf, b.Length);
    CDS cds = new CDS();
    cds.dwData = (IntPtr)0x4B440101;
    cds.cbData = b.Length;
    cds.lpData = buf;
    IntPtr p = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(CDS)));
    Marshal.StructureToPtr(cds, p, false);
    UIntPtr res;
    SendMessageTimeoutW(target, 0x004A /*WM_COPYDATA*/, IntPtr.Zero, p, 0x0002, 2000, out res);
    Marshal.FreeHGlobal(p);
    Marshal.FreeHGlobal(buf);
  }
  public static void Menu(IntPtr target, int id) {
    SendMessageW(target, 0x0111 /*WM_COMMAND*/, (IntPtr)id, IntPtr.Zero);
  }
}
"@

$bin = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build-windows-msvc-debug\bin\Debug"

# testtarget menu ids, from tools/testtarget/main.cpp
$M = @{
  ThemeDark = 40001; ThemeLight = 40002; FontLarge = 40003; FontSmall = 40004
  CaretBlink = 40010; CaretSteady = 40011; CaretFocusPane = 40012
  ScrollNone = 40020; ScrollOnInject = 40021
  LogClear = 40030
  TextClear = 40040; TextMidLine = 40041; TextLongLine = 40042; TextManyLines = 40043; TextRtl = 40044
}

# One entry per scenario this harness can arrange without changing the display configuration.
# S13 (mixed DPI), S14 (multi-monitor), S15 (HDR) and S18 (elevated) are deliberately absent:
# they need machine state I will not change unattended. S05/S06 need an injection text other than
# REF-1, which the probe does not accept. S09 needs an autocomplete popup, which testtarget has no
# way to raise.
$plan = @(
  @{ id = "S01"; control = "edit-multiline"; menu = @($M.TextClear);                     note = "empty field" }
  @{ id = "S02"; control = "edit-multiline"; menu = @($M.TextMidLine);                   note = "mid-line insertion" }
  @{ id = "S03"; control = "edit-multiline"; menu = @($M.TextMidLine); select = "10:20"; note = "selection replacement" }
  @{ id = "S04"; control = "edit-multiline"; menu = @($M.TextLongLine);                  note = "line wrap (multiline control wraps a long line)" }
  @{ id = "S07"; control = "edit-multiline"; menu = @($M.TextManyLines, $M.ScrollOnInject); note = "vertical scroll on inject" }
  @{ id = "S08"; control = "edit-single";    menu = @($M.TextLongLine);                  note = "horizontal scroll, single-line ES_AUTOHSCROLL" }
  @{ id = "S10"; control = "self-caret-pane";menu = @($M.CaretSteady);                   note = "steady (non-blinking) self-drawn caret" }
  @{ id = "S11"; control = "edit-multiline"; menu = @($M.TextMidLine, $M.ThemeDark);     note = "dark theme" }
  @{ id = "S12"; control = "edit-multiline"; menu = @($M.TextMidLine, $M.ThemeLight);    note = "light theme" }
  @{ id = "S16"; control = "edit-rtl";       menu = @($M.TextRtl);                       note = "RTL text" }
  @{ id = "S17"; control = "edit-password";  menu = @();                                 note = "password field - refusal is the behaviour under test" }
)

if (-not (Get-Process kd_testtarget -ErrorAction SilentlyContinue)) {
  Start-Process -FilePath "$bin\kd_testtarget.exe" | Out-Null
  Start-Sleep -Seconds 2
}
$t = [FG]::Find("KdTestTargetWindow")
if ($t -eq [IntPtr]::Zero) { Write-Output "no testtarget window"; exit 1 }

foreach ($cell in $plan) {
  if ($Only -ne "" -and $cell.id -ne $Only) { continue }

  # Arrange the scenario BEFORE taking focus for the injection, so nothing about the setup
  # lands in the diff.
  [SC]::Command($t, "focus:" + $cell.control)
  foreach ($m in $cell.menu) { [SC]::Menu($t, $m); Start-Sleep -Milliseconds 120 }
  [SC]::Command($t, "focus:" + $cell.control)
  if ($cell.ContainsKey("select")) {
    $parts = $cell.select.Split(":")
    [SC]::Command($t, "select:" + $parts[0] + ":" + $parts[1])
  }
  [SC]::Menu($t, $M.LogClear)
  Start-Sleep -Milliseconds 300

  [void][FG]::Force($t)
  Start-Sleep -Milliseconds 400
  $cls = [FG]::ClassOf([FG]::GetForegroundWindow())
  if ($cls -ne "KdTestTargetWindow") {
    Write-Output "[$($cell.id)] REFUSING: foreground is '$cls'"
    continue
  }

  Write-Output "=== $($cell.id) : $($cell.note)"
  # One probe invocation per repetition, with the scenario re-arranged in between. The probe's
  # own --repeat would leave the previous repetition's text in the control, so by rep 2 an
  # "empty field" is not empty, the line has wrapped and the control has scrolled - measuring a
  # different scenario under the first one's name.
  for ($i = 1; $i -le $Repeat; $i++) {
    [SC]::Command($t, "focus:" + $cell.control)
    foreach ($m in $cell.menu) { [SC]::Menu($t, $m); Start-Sleep -Milliseconds 100 }
    [SC]::Command($t, "focus:" + $cell.control)
    if ($cell.ContainsKey("select")) {
      $parts = $cell.select.Split(":")
      [SC]::Command($t, "select:" + $parts[0] + ":" + $parts[1])
    }
    Start-Sleep -Milliseconds 250
    [void][FG]::Force($t)
    Start-Sleep -Milliseconds 300
    if ([FG]::ClassOf([FG]::GetForegroundWindow()) -ne "KdTestTargetWindow") {
      Write-Output "  REFUSING rep ${i}: foreground lost"
      continue
    }
    & "$bin\kd_probe.exe" --app A09 --scenario $cell.id --inject sendinput --text $Text --require-exe kd_testtarget.exe --countdown 1 --repeat 1 --notes "automated sweep rep $i; control=$($cell.control); $($cell.note); state re-armed before this repetition; foreground verified"
  }
}

# Leave the target in a neutral state.
[SC]::Menu($t, $M.ThemeLight)
[SC]::Menu($t, $M.ScrollNone)
[SC]::Menu($t, $M.CaretBlink)
