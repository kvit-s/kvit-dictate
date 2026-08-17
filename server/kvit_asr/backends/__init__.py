"""Backend registry — lazy by construction.

**Importing this package must never import a model runtime.** plan.md §1.1 puts one backend per
process precisely because every candidate has a different runtime — NeMo, `transformers`,
CTranslate2, ONNX Runtime — and bench/CANDIDATES.md §3 turns that into four separate locked
environments. The consequence for this file is a hard rule:

    A backend module is imported only when that backend is asked for, and an ImportError while
    doing so disables **that backend**, not the server.

So there is no ``from .mock import MockBackend`` here and there must never be one for any future
backend. The dict below maps names to ``"module:attribute"`` strings that ``load_backend``
resolves through ``importlib`` at call time. ``base`` is the one exception — it is stdlib-only by
policy (see its module docstring) and is re-exported so the rest of the server can import the
contract without knowing which backend will satisfy it.

The name→interpreter mapping is **not** here: plan.md §1.1 is explicit that *"a Python-side model
registry cannot enumerate models that only exist under a different interpreter"*, so which
executable to launch lives in the C++-side ``models.json``. This registry only enumerates what is
loadable **in the interpreter currently running**, which is exactly the question a sidecar needs
answered about itself.
"""

from __future__ import annotations

import importlib
from typing import Optional

from .base import (
    SAMPLE_RATE,
    CHANNELS,
    BYTES_PER_SAMPLE,
    Backend,
    BackendCapabilities,
    BackendDecodeError,
    BackendError,
    BackendLoadError,
    BackendStateError,
    Hypothesis,
    ProgressFn,
    PseudoStreamingBackend,
    ResetReason,
    StreamingMode,
    bytes_to_ms,
    ms_to_bytes,
    pcm_peak,
    pcm_samples,
    utf8_len,
)

__all__ = [
    "SAMPLE_RATE",
    "CHANNELS",
    "BYTES_PER_SAMPLE",
    "Backend",
    "BackendCapabilities",
    "BackendDecodeError",
    "BackendError",
    "BackendLoadError",
    "BackendStateError",
    "BackendUnavailable",
    "Hypothesis",
    "ProgressFn",
    "PseudoStreamingBackend",
    "ResetReason",
    "StreamingMode",
    "bytes_to_ms",
    "ms_to_bytes",
    "pcm_peak",
    "pcm_samples",
    "utf8_len",
    "REGISTRY",
    "register",
    "backend_names",
    "load_backend",
    "create_backend",
    "probe",
]


class BackendUnavailable(BackendLoadError):
    """The backend exists but cannot be loaded in this interpreter.

    Distinct from ``BackendLoadError`` (the weights are missing / the GPU is out of memory)
    because the operator's next action is different: this one means "wrong environment", and
    bench/CANDIDATES.md §3 lists which env each backend needs. Both map to ``model_load_failed``
    on the wire — docs/protocol.md §11.3 lists "import failure" under that code explicitly — so
    the distinction is for humans reading logs, not for the client.
    """

    wire_code = "model_load_failed"


# name -> "module:attribute". Module paths starting with "." are resolved relative to this
# package. Only the mock is built in, because it is the only backend whose module exists today;
# bench/CANDIDATES.md §4 names the four Phase-1 adapters (sherpa-onnx Nemotron, sherpa-onnx
# Nemotron-3.5, sherpa-onnx zipformer, whisper-large-v3-turbo through transformers) and each
# registers itself here when its module lands. Listing a module that does not exist would turn a
# missing file into a runtime ImportError that reads like a missing dependency, which is exactly
# the confusion this registry is meant to remove.
REGISTRY: dict[str, str] = {
    "mock": ".mock:MockBackend",
}


def register(name: str, target: str) -> None:
    """Add or replace a registry entry. ``target`` is ``"module:attribute"``.

    Exists so a backend that lives outside this package — a spike, a bench-only adapter, a
    machine-specific variant — can be made available without editing this file.
    """
    if ":" not in target:
        raise ValueError(f"backend target must be 'module:attribute', got {target!r}")
    REGISTRY[name] = target


def backend_names() -> tuple[str, ...]:
    """Registered names. Says nothing about whether they can actually load — see ``probe``."""
    return tuple(sorted(REGISTRY))


def load_backend(name: str) -> type:
    """Resolve a registered name to its backend class, importing it now.

    Raises ``BackendUnavailable`` if the module cannot be imported, with the underlying error
    preserved in the message and as ``__cause__``. That is the whole point: on a machine with no
    ``onnxruntime`` this must be a legible "the onnx backend needs envs/onnx" rather than a
    traceback out of a transitive import.
    """
    try:
        target = REGISTRY[name]
    except KeyError:
        raise BackendUnavailable(
            f"unknown backend {name!r}; registered: {', '.join(backend_names()) or '(none)'}"
        ) from None

    module_path, _, attr = target.partition(":")
    package = __name__ if module_path.startswith(".") else None
    try:
        module = importlib.import_module(module_path, package=package)
    except ImportError as exc:
        raise BackendUnavailable(
            f"backend {name!r} is registered but its runtime is not importable "
            f"({module_path}: {exc}). See bench/CANDIDATES.md §3 for the environment it needs."
        ) from exc

    try:
        cls = getattr(module, attr)
    except AttributeError as exc:
        raise BackendUnavailable(
            f"backend {name!r}: {module_path} has no attribute {attr!r}"
        ) from exc

    if not (isinstance(cls, type) and issubclass(cls, Backend)):
        raise BackendUnavailable(
            f"backend {name!r}: {target} is not a Backend subclass"
        )
    return cls


def create_backend(name: str, *args, **kwargs) -> Backend:
    """``load_backend(name)(*args, **kwargs)``. Does not call ``load()``."""
    return load_backend(name)(*args, **kwargs)


def probe(name: str) -> Optional[str]:
    """Return ``None`` if the backend imports, or the reason it does not.

    For a diagnostics command that lists what this interpreter can actually run. Never raises,
    because the caller is enumerating and one broken entry must not end the enumeration.
    """
    try:
        load_backend(name)
    except Exception as exc:  # deliberately broad: a third-party module can raise anything
        return str(exc)
    return None
