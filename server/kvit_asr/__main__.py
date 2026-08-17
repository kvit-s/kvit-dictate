r"""Process entry point: `python -m kvit_asr --pipe <name> --backend <id> [...]`.

Launched by `kvit-dictate.exe`, never by a human in normal operation. The parent creates the
named pipe first, generates its per-launch unpredictable name, applies the DACL, and only then
spawns this process with that name on the command line (docs/protocol.md §2.4). The by-hand
form exists for debugging and is documented in server/README.md.

**There is no port and never will be.** plan.md §1.2 chose a named pipe precisely so that the
first run of a privacy tool does not produce a Windows Firewall prompt, and so that there is no
port collision and no discovery dance. `--port` and `--host` are accepted by the parser only so
that passing them fails with that sentence instead of "unrecognised arguments".

**One backend per process.** `--backend` is read once. Changing model means the parent starts a
new process under that model's interpreter, because the model→interpreter mapping lives in the
C++-side `models.json` and no single interpreter can import every candidate's runtime
(plan.md §1.1, §5). The long-form version of this argument, and the reason a registry here
would be wrong rather than merely unnecessary, is at the top of `kvit_asr/server.py`.

**stdout is not the transport.** docs/protocol.md §2.1 rejects stdio precisely because the
Python runtime, its logging, and any C extension it loads write to stdout/stderr
unpredictably — one stray `print` would corrupt a binary frame stream. Diagnostics therefore go
to stderr as one JSON object per line, and anything a backend prints to stdout is noise the
parent may discard.
"""

from __future__ import annotations

import json
import sys
import time

# Captured before the heavy imports below so that, on platforms or builds where
# GetProcessTimes is unavailable, `warm_ms` still has an origin close to process start.
# `server.process_age_ms()` prefers the real process creation time and says which it used.
_ENTRY_MONOTONIC = time.monotonic()


def main() -> int:
    try:
        from kvit_asr.server import main as server_main
    except Exception as exc:  # pragma: no cover - exercised only by a broken checkout
        # No pipe yet, so there is no peer to send an `error` frame to. The only honest thing is
        # one structured line naming what failed, in the same format as every other log line, so
        # the parent's log capture can read it.
        sys.stderr.write(
            json.dumps(
                {
                    "lvl": "error",
                    "ev": "import_failed",
                    "exc_type": type(exc).__name__,
                    "exc_msg": str(exc),
                    "note": "the sidecar could not import its own modules; the parent will "
                    "see a spawn/handshake timeout rather than a protocol rejection",
                }
            )
            + "\n"
        )
        sys.stderr.flush()
        return 7  # server.EXIT_INTERNAL, spelled literally because the import of it failed
    return server_main(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
