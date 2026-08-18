Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class FG {
  delegate bool P(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(P cb, IntPtr l);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);

  public static IntPtr Find(string cls) {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h,l) => { var sb=new StringBuilder(128); GetClassNameW(h,sb,128);
      if (sb.ToString()==cls) { f=h; return false; } return true; }, IntPtr.Zero);
    return f; }

  public static string ClassOf(IntPtr h) { var sb=new StringBuilder(128); GetClassNameW(h,sb,128); return sb.ToString(); }

  // Windows refuses SetForegroundWindow from a process that does not own the foreground.
  // Attaching our input queue to the current foreground thread lifts that restriction for the
  // duration of the attach; this is the documented technique, not a trick.
  public static bool Force(IntPtr target) {
    ShowWindow(target, 9);
    IntPtr fg = GetForegroundWindow();
    if (fg == target) return true;
    uint fgThread = GetWindowThreadProcessId(fg, IntPtr.Zero);
    uint us = GetCurrentThreadId();
    bool attached = (fgThread != 0 && fgThread != us) && AttachThreadInput(us, fgThread, true);
    try {
      BringWindowToTop(target);
      SetForegroundWindow(target);
    } finally {
      if (attached) AttachThreadInput(us, fgThread, false);
    }
    return GetForegroundWindow() == target;
  }
}
"@
