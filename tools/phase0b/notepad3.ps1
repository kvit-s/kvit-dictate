# Measure A10 (Notepad3) — the one real application on this machine that can be measured without
# an operator, because it is the only one that exposes a system caret (docs/supported-targets.md
# §3.4, and tools/phase0b/caret-survey.ps1 is what established that).
#
# ─── This types into a real editor. Read this paragraph before running it. ───────────────────
#
# Notepad3 is multi-instance and there are usually several windows open, some holding unsaved
# work. `--require-exe Notepad3.exe` does NOT protect those: it is an executable-level guard and
# every window of the editor passes it. So this script:
#
#   1. records the set of Notepad3 windows that already exist,
#   2. launches a new one and identifies it by difference,
#   3. refuses to continue unless that window's title is exactly "Untitled - Notepad3" — no
#      asterisk, which is Notepad3's unsaved-changes marker,
#   4. pins its HWND and passes it to every probe invocation as --require-hwnd, and
#   5. re-checks the foreground against that same HWND immediately before every setup keystroke
#      it sends itself.
#
# Nothing is saved and the window is left open at the end for inspection. Close it without saving.

param([int]$Repeat = 3, [string]$Only = "")

$here = Split-Path -Parent $PSCommandPath
. "$here\focus.ps1"

$bin = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) "build-windows-msvc-debug\bin\Debug"
$exe = "C:\Program Files\Notepad3\Notepad3.exe"
if (-not (Test-Path $exe)) { Write-Output "Notepad3 not found at $exe"; exit 1 }

Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class Keys {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
    public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT {
    public uint type; public KEYBDINPUT ki; public int pad1; public int pad2; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int cb);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();

  const uint KEYEVENTF_KEYUP = 0x0002, KEYEVENTF_UNICODE = 0x0004;

  static INPUT Unit(ushort vk, ushort scan, uint flags) {
    INPUT i = new INPUT(); i.type = 1;
    i.ki.wVk = vk; i.ki.wScan = scan; i.ki.dwFlags = flags;
    return i;
  }

  // Every call re-checks the foreground itself. The guard has to be inside the thing that sends
  // the events, not in the caller: a focus change between the caller's check and the send is
  // exactly how an earlier sweep typed a reference string into the operator's editor.
  static bool Guarded(IntPtr want) { return GetForegroundWindow() == want; }

  public static bool Text(IntPtr want, string s) {
    if (!Guarded(want)) return false;
    foreach (char c in s) {
      if (!Guarded(want)) return false;
      INPUT[] b = new INPUT[2];
      b[0] = Unit(0, (ushort)c, KEYEVENTF_UNICODE);
      b[1] = Unit(0, (ushort)c, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
      SendInput(2, b, Marshal.SizeOf(typeof(INPUT)));
      System.Threading.Thread.Sleep(1);
    }
    return true;
  }

  // vks: virtual-key codes pressed and released in order. mod is an optional held modifier.
  public static bool Chord(IntPtr want, ushort mod, ushort[] vks) {
    if (!Guarded(want)) return false;
    if (mod != 0) { INPUT[] d = new INPUT[1]; d[0] = Unit(mod, 0, 0); SendInput(1, d, Marshal.SizeOf(typeof(INPUT))); }
    foreach (ushort vk in vks) {
      INPUT[] b = new INPUT[2];
      b[0] = Unit(vk, 0, 0);
      b[1] = Unit(vk, 0, KEYEVENTF_KEYUP);
      SendInput(2, b, Marshal.SizeOf(typeof(INPUT)));
      System.Threading.Thread.Sleep(3);
    }
    if (mod != 0) { INPUT[] u = new INPUT[1]; u[0] = Unit(mod, 0, KEYEVENTF_KEYUP); SendInput(1, u, Marshal.SizeOf(typeof(INPUT))); }
    return true;
  }
}
public class Win {
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  public static string Title(IntPtr h) { var sb=new StringBuilder(256); GetWindowTextW(h,sb,256); return sb.ToString(); }
}
"@

$VK_A = 0x41; $VK_DELETE = 0x2E; $VK_HOME = 0x24; $VK_RIGHT = 0x27; $VK_END = 0x23; $VK_CONTROL = 0x11

# ── 1-3. Get a window we are certain is ours, and certain is empty ──────────────────────────
$before = @(Get-Process -Name Notepad3 -ErrorAction SilentlyContinue |
            ForEach-Object { $_.MainWindowHandle } | Where-Object { $_ -ne 0 })
Write-Output "Notepad3 windows already open: $($before.Count) — none of them will be touched."

Start-Process -FilePath $exe | Out-Null
Start-Sleep -Seconds 3

$after = @(Get-Process -Name Notepad3 -ErrorAction SilentlyContinue |
           ForEach-Object { $_.MainWindowHandle } | Where-Object { $_ -ne 0 })
$new = @($after | Where-Object { $before -notcontains $_ })
if ($new.Count -ne 1) {
  Write-Output "REFUSING: expected exactly one new Notepad3 window, found $($new.Count). Nothing was typed."
  exit 2
}
$target = $new[0]
$title = [Win]::Title($target)
if ($title -ne "Untitled - Notepad3") {
  Write-Output "REFUSING: the new window's title is '$title', expected exactly 'Untitled - Notepad3'."
  Write-Output "An asterisk means unsaved changes, so this is not a window we may type into. Nothing was typed."
  exit 2
}
$hwndHex = "0x" + ("{0:X}" -f [int64]$target)
Write-Output "target window: $hwndHex  '$title'  (pinned; every injection carries --require-hwnd)"

# ── The scenarios this can arrange ──────────────────────────────────────────────────────────
# Far fewer than A09's: there is no side channel, so the document can only be arranged by typing
# into it, and only the scenarios reachable that way exist here.
$plan = @(
  @{ id = "S01"; note = "empty document"; setup = { param($h) $true } }
  @{ id = "S02"; note = "mid-line insertion, caret 20 into a 45-char line"
     setup = {
       param($h)
       if (-not [Keys]::Text($h, "the quick brown fox | jumps over the lazy dog")) { return $false }
       Start-Sleep -Milliseconds 200
       if (-not [Keys]::Chord($h, 0, @($VK_HOME))) { return $false }
       [Keys]::Chord($h, 0, @($VK_RIGHT) * 20)
     } }
)

foreach ($cell in $plan) {
  if ($Only -ne "" -and $cell.id -ne $Only) { continue }
  Write-Output ""
  Write-Output "=== A10/$($cell.id) : $($cell.note)"
  for ($i = 1; $i -le $Repeat; $i++) {
    [void][FG]::Force($target)
    Start-Sleep -Milliseconds 400
    if ([FG]::GetForegroundWindow() -ne $target) {
      Write-Output "  REFUSING rep ${i}: could not bring the pinned window to the foreground."
      continue
    }
    # Empty the document, then arrange. Ctrl+A / Delete is why the HWND check lives inside Keys.
    if (-not [Keys]::Chord($target, $VK_CONTROL, @($VK_A))) { Write-Output "  REFUSING rep ${i}: focus lost"; continue }
    Start-Sleep -Milliseconds 80
    if (-not [Keys]::Chord($target, 0, @($VK_DELETE))) { Write-Output "  REFUSING rep ${i}: focus lost"; continue }
    Start-Sleep -Milliseconds 150
    if (-not (& $cell.setup $target)) { Write-Output "  REFUSING rep ${i}: focus lost during setup"; continue }
    Start-Sleep -Milliseconds 400

    & "$bin\kd_probe.exe" --app A10 --scenario $cell.id --inject sendinput --text ref2 `
        --require-hwnd $hwndHex --require-exe Notepad3.exe --countdown 1 --repeat 1 `
        --notes "A10 Notepad3 rep $i; $($cell.note); fresh Untitled window pinned by hwnd; ground truth from the system caret"
  }
}

Write-Output ""
Write-Output "Done. The window $hwndHex is left open with test text in it — close it WITHOUT saving."
