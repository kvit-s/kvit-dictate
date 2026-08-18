# The A09 scenario plan and the code that arranges it — dot-sourced by sweep.ps1, which measures,
# and by verify-arrange.ps1, which checks the arrangement without injecting anything. They share
# this file so that what gets verified is literally what gets measured; when the two were written
# out separately the sweep drifted from its own description and nobody noticed for eleven cells.

$here = Split-Path -Parent $PSCommandPath
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

# The baseline every cell starts from. Anything a cell switches on is switched off here before the
# next cell — and before each repetition of the same cell, because a repetition inherits the
# previous one's state exactly as a cell inherits the previous cell's. TextClear is last of the
# five: it empties every control, so a cell's own text menu item runs after it and wins.
#
# Not having this is what produced the worst of the first eleven cells. S07 turned on "scroll the
# pane on each insertion", S10 ran next and turned it off nowhere, and S10's 25 injected
# characters walked the pane's content 25 line heights off the top of the window: ground truth
# came back at y = -226, and repetitions 2 and 3 recorded no diff at all because there was
# nothing left on screen to change.
$Baseline = @($M.ScrollNone, $M.CaretBlink, $M.ThemeLight, $M.FontSmall, $M.TextClear)

# One entry per scenario this harness can arrange without changing the display configuration.
# S13 (mixed DPI), S14 (multi-monitor), S15 (HDR) and S18 (elevated) are deliberately absent:
# they need machine state that will not be changed unattended. S09 needs an autocomplete popup,
# which testtarget has no way to raise.
#
# `expect` is what verify-arrange.ps1 checks the arranged state against, so a cell that stops
# arranging what its name says fails a check rather than quietly measuring something else.
# `text` overrides the sweep's -Text for that cell, for the cells whose scenario is defined by
# WHICH string is injected rather than by how the control is arranged.
#
#   focus       the control that must hold the focus
#   caret_at    "end" | "start" | an integer offset — where in `text` the caret must sit
#   sel         "<a>:<b>" instead of caret_at, for the one cell that wants a live selection
#   text_head   the text the control must contain, matched as a prefix
$plan = @(
  @{ id = "S01"; control = "edit-multiline"; menu = @($M.TextClear);   note = "empty field"
     expect = @{ focus = "edit-multiline"; caret_at = 0; text_head = "" } }

  @{ id = "S02"; control = "edit-multiline"; menu = @($M.TextMidLine); note = "mid-line insertion"
     expect = @{ focus = "edit-multiline"; caret_at = 20; text_head = "the quick brown fox | jumps" } }

  @{ id = "S03"; control = "edit-multiline"; menu = @($M.TextMidLine); select = "10:20"
     note = "selection replacement"
     # A live selection, not a caret: the ten characters at [10,20) must still be selected when
     # the injection arrives, because what is under test is the target replacing them.
     expect = @{ focus = "edit-multiline"; sel = "10:20"; text_head = "the quick brown fox | jumps" } }

  @{ id = "S04"; control = "edit-multiline"; menu = @($M.TextLongLine)
     note = "line wrap (multiline control wraps a long line)"
     expect = @{ focus = "edit-multiline"; caret_at = "end"; text_head = "C:\apps\llama-swap" } }

  # No ScrollOnInject here any more: that menu item only ever drove the self-drawn pane, so
  # against edit-multiline it did nothing except leak into whichever cell ran next. The scroll now
  # comes from the arrangement itself — the control is filled past its height, scrolled to the
  # bottom, and its last line padded so REF-2 wraps and pushes everything up by one line.
  @{ id = "S07"; control = "edit-multiline"; menu = @($M.TextManyLines)
     note = "vertical scroll: injection wraps the padded last line of a control already at its bottom"
     expect = @{ focus = "edit-multiline"; caret_at = "end"; text_head = "line 0 " } }

  @{ id = "S08"; control = "edit-single";    menu = @($M.TextLongLine)
     note = "horizontal scroll, single-line ES_AUTOHSCROLL"
     expect = @{ focus = "edit-single"; caret_at = "end"; text_head = "C:\apps\llama-swap" } }

  # S10 is deliberately S01's arrangement with one variable changed: an empty field, a clean
  # append at the start of it, and the ONLY difference is that this control never calls
  # CreateCaret. S01 measured 0.07 line heights with a system caret available; whatever S10
  # measures is therefore attributable to source C being the only anchor, not to the insertion
  # position. Anything mid-line here would fold S02's reflow error into the answer.
  @{ id = "S10"; control = "self-caret-pane"; menu = @($M.TextClear, $M.CaretSteady)
     note = "steady (non-blinking) self-drawn caret; empty pane, so this is S01 with source A removed"
     expect = @{ focus = "self-caret-pane"; caret_at = 0; text_head = ""; no_system_caret = $true } }

  @{ id = "S11"; control = "edit-multiline"; menu = @($M.TextMidLine, $M.ThemeDark); note = "dark theme"
     expect = @{ focus = "edit-multiline"; caret_at = 20; text_head = "the quick brown fox | jumps"; theme = "dark" } }

  @{ id = "S12"; control = "edit-multiline"; menu = @($M.TextMidLine, $M.ThemeLight); note = "light theme"
     expect = @{ focus = "edit-multiline"; caret_at = 20; text_head = "the quick brown fox | jumps"; theme = "light" } }

  # REF-3, not the sweep's default: S16 is "the insertion point is at the LEFT edge of the
  # changed region", and an LTR injection cannot produce that however RTL the control is. The
  # earlier S16 figures measured an LTR run appended to RTL text and said so. This is the cell
  # that makes docs/supported-targets.md §9 question 8 answerable.
  @{ id = "S16"; control = "edit-rtl";       menu = @($M.TextRtl);   text = "ref3"
     note = "RTL text injected into an RTL control (REF-3)"
     expect = @{ focus = "edit-rtl"; caret_at = "end"; text_head = "" } }

  # S05 and S06 test one rule from two directions (plan.md §1.5): an empty or near-empty diff must
  # NOT be read as an injection failure. Both use the mid-line arrangement so there is surrounding
  # text for a reflow to be visible against if one happens.
  @{ id = "S05"; control = "edit-multiline"; menu = @($M.TextMidLine); text = "ref4"
     note = "newline-only commit (REF-4); layout below the caret shifts, the caret line barely changes"
     expect = @{ focus = "edit-multiline"; caret_at = 20; text_head = "the quick brown fox | jumps" } }

  @{ id = "S06"; control = "edit-multiline"; menu = @($M.TextMidLine); text = "ref5"
     note = "whitespace-only commit (REF-5); an invisible change that must not read as failure"
     expect = @{ focus = "edit-multiline"; caret_at = 20; text_head = "the quick brown fox | jumps" } }

  @{ id = "S17"; control = "edit-password";  menu = @();
     note = "password field - refusal is the behaviour under test"
     expect = @{ focus = "edit-password"; caret_at = 0; text_head = "" } }
)

function Find-TestTarget {
  if (-not (Get-Process kd_testtarget -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath "$bin\kd_testtarget.exe" | Out-Null
    Start-Sleep -Seconds 2
  }
  $t = [FG]::Find("KdTestTargetWindow")
  if ($t -eq [IntPtr]::Zero) { throw "no testtarget window" }
  return $t
}

# Arrange one scenario from the baseline and leave the target focused on its control. Every step
# goes through the side channel or the menu; nothing here synthesises input, so none of it can
# land in the diff the probe is about to take, and none of it needs the foreground.
function Arrange($t, $cell) {
  foreach ($m in $Baseline) { [SC]::Menu($t, $m); Start-Sleep -Milliseconds 60 }
  [SC]::Command($t, "focus:" + $cell.control)
  foreach ($m in $cell.menu) { [SC]::Menu($t, $m); Start-Sleep -Milliseconds 100 }
  # Focus again: filling the text can move it, and the select below acts on GetFocus().
  [SC]::Command($t, "focus:" + $cell.control)
  if ($cell.ContainsKey("select")) {
    $parts = $cell.select.Split(":")
    [SC]::Command($t, "select:" + $parts[0] + ":" + $parts[1])
  }
}
