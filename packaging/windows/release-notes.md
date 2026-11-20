Press **Win+Y**, talk, and the text appears in whichever window has focus. Windows 10 (1809+) and
11, x64.

| File | What it is |
|---|---|
| `kvit-dictate-…-setup.exe` | Installer. Per-user, into `%LOCALAPPDATA%\Programs\kvit-dictate`, no administrator prompt. |
| `kvit-dictate-…-win-x64.zip` | Portable copy. Unpack anywhere and run `kvit-dictate.exe`. |
| `SHA256SUMS.txt` | Checksums of both. |

After installing, open **Settings → Server** and choose where the speech is recognised:

* **This PC**: Microsoft's on-device speech model, so nothing leaves the computer. Needs Windows 11
  24H2 or later. The model downloads once, through Windows Update, when you press **Test**.
* **A speech service** you run or rent: anything that serves `/v1/audio/transcriptions`. See
  [`docs/setup.md`](https://github.com/kvit-s/kvit-dictate/blob/main/docs/setup.md).

**These builds are not code-signed, so SmartScreen will warn you.** The program hooks the keyboard
and types into other windows, which is what SmartScreen warns about. The
[README](https://github.com/kvit-s/kvit-dictate#what-it-does-to-your-machine) explains exactly
what it does to your machine. These files were built by this repository's `Release` workflow,
from the tagged commit.
