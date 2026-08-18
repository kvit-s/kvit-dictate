# Which real applications expose a system caret — and are therefore measurable unattended?
#
# docs/supported-targets.md §3.4: a target backed by a real system caret is self-measuring,
# because GetGUIThreadInfo read from outside the process returns what the target reports about
# itself (validated against tools/testtarget on 7/7 scenarios). A target that draws its own caret
# is not, and needs the operator-annotation step.
#
# Which is which is a property of the application, and guessing it wrong wastes a measurement
# session. This script asks. It **types nothing, captures nothing and changes nothing** — it
# brings a window to the foreground, reads GetGUIThreadInfo, and moves on. Safe to run any time.
#
#   tools\phase0b\caret-survey.ps1                  survey whatever is already running
#   tools\phase0b\caret-survey.ps1 -Exe notepad3    survey one process by name
#
# **It reports the caret of whatever control has focus when the window is raised**, which for most
# applications is not a text field at all. "no caret" here therefore means "no caret at this
# window's default focus", NOT "this application never exposes one" — Chrome's omnibox and File
# Explorer's search box would each need their focus chord sent first. Read it as a lower bound.
#
# It does steal the foreground, one window at a time, for about a third of a second each: the
# caret only exists while the owning thread is active, so there is no way to read it from the
# background. Nothing is typed and nothing is saved.

param([string]$Exe = "")

$here = Split-Path -Parent $PSCommandPath
. "$here\focus.ps1"

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class Caret {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
  [StructLayout(LayoutKind.Sequential)]
  public struct GUITHREADINFO {
    public int cbSize; public int flags; public IntPtr hwndActive; public IntPtr hwndFocus;
    public IntPtr hwndCapture; public IntPtr hwndMenuOwner; public IntPtr hwndMoveSize;
    public IntPtr hwndCaret; public RECT rcCaret;
  }
  [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO gti);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);

  // Returns "w,h @ x,y (class)" for a non-empty caret, or "" when the target has none.
  public static string Probe(IntPtr hwnd) {
    uint pid; uint tid = GetWindowThreadProcessId(hwnd, out pid);
    if (tid == 0) return "";
    GUITHREADINFO g = new GUITHREADINFO();
    g.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
    if (!GetGUIThreadInfo(tid, ref g)) return "";
    int w = g.rcCaret.right - g.rcCaret.left, h = g.rcCaret.bottom - g.rcCaret.top;
    if (w <= 0 && h <= 0) return "";
    var cls = new StringBuilder(128);
    if (g.hwndFocus != IntPtr.Zero) GetClassNameW(g.hwndFocus, cls, 128);
    return w + "x" + h + " at " + g.rcCaret.left + "," + g.rcCaret.top + "  focus class=" + cls.ToString();
  }
  public static string Title(IntPtr h) { var sb=new StringBuilder(256); GetWindowTextW(h,sb,256); return sb.ToString(); }
}
"@

$procs = if ($Exe -ne "") { Get-Process -Name $Exe -ErrorAction SilentlyContinue }
         else { Get-Process | Where-Object { $_.MainWindowHandle -ne 0 } }

if (-not $procs) { Write-Output "no matching process with a window"; exit 1 }

Write-Output ""
Write-Output "Caret survey — nothing is typed, nothing is captured."
Write-Output "A target with a caret can be measured unattended (§3.4); one without needs an operator."
Write-Output ""

foreach ($p in $procs) {
  $h = $p.MainWindowHandle
  if ($h -eq 0) { continue }
  $title = [Caret]::Title($h)
  # The caret only exists while the thread is active, so the window has to be foreground for the
  # read to mean anything. This is the one intrusive thing here and it is reversible.
  [void][FG]::Force($h)
  Start-Sleep -Milliseconds 350
  $c = [Caret]::Probe($h)
  $verdict = if ($c -ne "") { "CARET   " } else { "no caret" }
  $short = if ($title.Length -gt 46) { $title.Substring(0,46) } else { $title }
  Write-Output ("{0}  {1,-16} {2,-48} {3}" -f $verdict, $p.ProcessName, $short, $c)
}
Write-Output ""
