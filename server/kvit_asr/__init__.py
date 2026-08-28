# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""kvit-dictate ASR sidecar — package root.

The sidecar core has **zero third-party dependencies**. Everything reachable from this module
runs on a bare stdlib CPython 3.12: transport, framing, JSON, the session loop, the stability
computation and the mock backend. Only individual model *backends* may import numpy /
onnxruntime / torch, and each such import lives inside the backend module rather than at package
import time — so a missing runtime disables one backend instead of breaking the server.

That rule is the reason this file imports `protocol` (stdlib only: json, unicodedata, enum) and
deliberately does **not** import `transport` (which binds kernel32 through ctypes) or anything
under `backends/`. Importing `kvit_asr` must stay cheap and must never fail because of a model
runtime.

Normative documents, in precedence order:
  1. docs/protocol.md — the frozen wire contract. Where this code and that document disagree,
     the document wins and this code is the bug.
  2. plan.md §1.2, §1.4 — architecture and the committed-prefix invariant.
"""

from __future__ import annotations

__all__ = ["__version__", "SERVER_ID", "PROTO_VERSION"]

__version__ = "0.1.0"

# `hello_ack.server` / `ready` diagnostics string. docs/protocol.md §6.2: diagnostics only, never
# parsed for behaviour by the peer.
SERVER_ID = f"kvit-asr/{__version__}"

# Re-exported for callers that only need the version number and should not have to know which
# module holds the constant table. The authoritative copy is protocol.PROTO_VERSION.
from .protocol import PROTO_VERSION  # noqa: E402  (placed after the docstring intentionally)
