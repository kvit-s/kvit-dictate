"""A stand-in for `tools/kvit-speech/kvit-speech.exe`, for the tests.

Speaks the frame protocol in `kvit_asr.backends.windows_speech`'s module docstring and nothing
else. It exists so the backend's process handling — startup, framing, timeouts, a helper that
dies, a helper that answers with an error — is tested on a machine with no .NET SDK, no Windows
App SDK and no speech model installed, which is every machine except the operator's.

Behaviour is chosen by argv so one file covers every case:

    --ready            send 'R' and serve normally (the default)
    --fail MESSAGE     send 'E' with MESSAGE at startup and exit, as an uninstalled model does
    --silent           send nothing at all and sit there, so the startup timeout is exercised
    --hang-on-audio    answer 'P' but never answer 'T', so the request timeout is exercised
    --die-on-audio     exit without answering a 'T'
    --error-on-audio M answer every 'T' with 'E' and message M
    --echo-length      answer 'T' with the audio's length in ms, so the window can be asserted on
    --text TEXT        answer every 'T' with TEXT (the default is a fixed sentence)
    --garbage-length   send a frame header claiming a length past the cap, then stop
"""

from __future__ import annotations

import struct
import sys
import time


def _write(stream, tag: bytes, payload: bytes = b"") -> None:
    stream.write(struct.pack("<I", len(payload) + 1) + tag + payload)
    stream.flush()


def _read_exactly(stream, n: int) -> bytes | None:
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def main(argv: list[str]) -> int:
    out = sys.stdout.buffer
    inp = sys.stdin.buffer

    def opt(name: str, default: str | None = None) -> str | None:
        return argv[argv.index(name) + 1] if name in argv else default

    if "--fail" in argv:
        _write(out, b"E", (opt("--fail") or "no model").encode("utf-8"))
        return 1
    if "--silent" in argv:
        time.sleep(30)
        return 0
    if "--garbage-length" in argv:
        out.write(struct.pack("<I", 0xFFFFFFF0))
        out.flush()
        time.sleep(30)
        return 0

    _write(out, b"R", b'{"model":"fake","proto":1,"native_folder":"C:\\\\fake\\\\winml"}')

    text = (opt("--text") or "the fake helper heard something").encode("utf-8")

    while True:
        head = _read_exactly(inp, 4)
        if head is None:
            return 0
        (length,) = struct.unpack("<I", head)
        body = _read_exactly(inp, length)
        if body is None:
            return 0
        tag, payload = body[:1], body[1:]

        if tag == b"Q":
            return 0
        if tag == b"P":
            _write(out, b"O")
            continue
        if tag != b"T":
            _write(out, b"E", b"unknown request tag")
            continue

        if "--hang-on-audio" in argv:
            time.sleep(30)
            return 0
        if "--die-on-audio" in argv:
            return 3
        if "--error-on-audio" in argv:
            _write(out, b"E", (opt("--error-on-audio") or "decode failed").encode("utf-8"))
            continue
        if "--echo-length" in argv:
            _write(out, b"O", f"{len(payload) // 32} ms".encode("utf-8"))
            continue
        _write(out, b"O", text)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
