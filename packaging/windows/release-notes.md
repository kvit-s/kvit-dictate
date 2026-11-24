Press **Win+Y**, talk, and the text appears in whichever window has focus. Windows 10 (1809+) and
11, x64.

| File | What it is |
|---|---|
| `kvit-dictate-…-setup.exe` | Installer. Per-user, into `%LOCALAPPDATA%\Programs\kvit-dictate`, no administrator prompt. |
| `kvit-dictate-…-win-x64.zip` | Portable copy. Unpack anywhere and run `kvit-dictate.exe`. |
| `SHA256SUMS.txt` | Checksums of both. |

**Updating.** Run the new installer over the old one; it closes the running copy itself. From
1.0.0-rc2 on, the app also finds new versions by itself: the tray's **Check for updates…** (or the
notification it shows) downloads the installer, checks its checksum, and installs it when you
press **Install now**. The portable ZIP is not updated in place; it offers the download page.

After installing, open **Settings → Server** and choose where the speech is recognised:

* **This PC**: NVIDIA's Nemotron speech model on your CPU, so nothing you say leaves the computer.
  Press **Download Nemotron model** once (728 MB, from Microsoft's model catalog), then **Test**.
  It replaces Microsoft's Windows speech model, which earlier releases used and which is gone.
* **A speech service** you run or rent: anything that serves `/v1/audio/transcriptions`. See
  [`docs/setup.md`](https://github.com/kvit-s/kvit-dictate/blob/main/docs/setup.md).

**These builds are not code-signed, so SmartScreen will warn you.** The program hooks the keyboard
and types into other windows, which is what SmartScreen warns about. The
[README](https://github.com/kvit-s/kvit-dictate#what-it-does-to-your-machine) explains exactly
what it does to your machine. These files were built by this repository's `Release` workflow,
from the tagged commit.
